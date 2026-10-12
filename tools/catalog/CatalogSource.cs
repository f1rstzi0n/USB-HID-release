/* 人工维护的 YAML 是唯一输入；CSV 和发布侧文件均为派生产物。 */
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.RepresentationModel;

internal sealed class CatalogSource
{
    private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
    private static readonly string[] Types = { "Undefined", "UINT8", "UINT16", "UINT32", "UINT64", "INT32", "FLOAT32", "FIXED16.16", "UFIXED16.16", "UFRAC16", "TEXT8", "TEXT16", "TEXT32", "COMPOSITE" };
    private static readonly int[] Sizes = { 0, 1, 2, 4, 8, 4, 4, 4, 4, 2, 8, 16, 32, 0 };
    private static readonly string[] Roles = {
        "status.product", "status.access-mode", "status.control-mode", "status.controller", "status.error-number", "status.error-code", "status.firmware",
        "button.open", "button.close", "button.learn", "button.zero-adjust", "button.restart", "led.open", "led.closed",
        "position.feedback", "position.target", "pressure.feedback", "pressure.target", "chart.position.feedback", "chart.position.target", "chart.pressure.feedback", "chart.pressure.target",
        "info.product-code", "info.device-type", "info.communication-address", "info.baud-rate", "info.control-mode", "info.function-mode", "info.alarm"
    };
    private sealed class Parameter
    {
        internal YamlNode Node;
        internal string Group, Name, Address, Type, Access, RuntimeAccess, Default, Min, Max, RuntimeMin, RuntimeMax, Unit, Enum, Widget, Remark;
        internal int TypeCode, Count, Size, Parent, Visible, RuntimeVisible;
        internal uint AddressValue;
        internal byte[] Initial;
    }
    private sealed class Group
    {
        internal int Id, Parent;
        internal string Key, Path, Chinese, English;
    }
    private sealed class Binding
    {
        internal string Role, Address;
        internal int Order;
    }
    private readonly string file;
    private readonly List<Parameter> parameters = new List<Parameter>();
    private readonly List<Group> groups = new List<Group>();
    private readonly List<string[]> tree = new List<string[]>();
    private readonly List<Binding> bindings = new List<Binding>();
    private readonly List<string[]> errors = new List<string[]>();
    private readonly HashSet<string> groupKeys = new HashSet<string>(StringComparer.Ordinal);
    private readonly Dictionary<string, Parameter> names = new Dictionary<string, Parameter>(StringComparer.Ordinal);
    private readonly HashSet<uint> addresses = new HashSet<uint>();
    private string catalogId, sourceVersion, firmwareVersion, catalogVersion, target, deviceName, sourceHash;
    private int deviceType, rangeOverrides;
    private long initialBytes;

    private CatalogSource(string file) { this.file = Path.GetFullPath(file); }

    internal static int Run(string[] args)
    {
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        if (args.Length % 2 != 0) throw new ArgumentException("Use source --input FILE [--output DIRECTORY].");
        for (int i = 0; i < args.Length; i += 2)
        {
            if ((args[i] != "--input" && args[i] != "--output") || options.ContainsKey(args[i]) || string.IsNullOrWhiteSpace(args[i + 1]))
                throw new ArgumentException("Unknown, duplicate or empty source option: " + args[i]);
            options.Add(args[i], args[i + 1]);
        }
        string input;
        if (!options.TryGetValue("--input", out input)) throw new ArgumentException("Missing --input.");
        var source = new CatalogSource(input);
        source.Load();
        string output;
        if (options.TryGetValue("--output", out output)) source.Export(output);
        Console.WriteLine("PASS source: {0} parameters, {1} groups, {2} tree records, {3} UI bindings, {4} errors.", source.parameters.Count, source.groups.Count, source.tree.Count, source.bindings.Count, source.errors.Count);
        return 0;
    }

    private InvalidDataException Error(YamlNode node, string path, string message)
    { return new InvalidDataException(string.Format(Invariant, "{0}:{1}:{2}: {3}: {4}", file, node.Start.Line, node.Start.Column, path, message)); }

    private Dictionary<string, YamlNode> Map(YamlNode node, string path, params string[] allowed)
    {
        var mapping = node as YamlMappingNode;
        if (mapping == null) throw Error(node, path, "Expected a mapping.");
        var result = new Dictionary<string, YamlNode>(StringComparer.Ordinal);
        foreach (var entry in mapping.Children)
        {
            string key = Text(entry.Key, path + ".<key>");
            if (!allowed.Contains(key)) throw Error(entry.Key, path + "." + key, "Unknown field.");
            if (result.ContainsKey(key)) throw Error(entry.Key, path + "." + key, "Duplicate field.");
            result.Add(key, entry.Value);
        }
        return result;
    }

    private YamlNode Required(Dictionary<string, YamlNode> map, string key, YamlNode owner, string path)
    {
        YamlNode value;
        if (!map.TryGetValue(key, out value)) throw Error(owner, path + "." + key, "Missing field.");
        return value;
    }

    private string Optional(Dictionary<string, YamlNode> map, string key, string path, string fallback = "")
    { YamlNode node; return map.TryGetValue(key, out node) ? Text(node, path + "." + key) : fallback; }

    private string Text(YamlNode node, string path)
    {
        var scalar = node as YamlScalarNode;
        if (scalar == null || scalar.Value == null || (scalar.Style == ScalarStyle.Plain && new[] { "", "~", "null", "Null", "NULL" }.Contains(scalar.Value)))
            throw Error(node, path, "Expected a scalar; quote an empty string explicitly.");
        if (scalar.Value.IndexOf('\0') >= 0 || scalar.Value.IndexOf('\r') >= 0 || scalar.Value.IndexOf('\n') >= 0)
            throw Error(node, path, "NUL and line breaks cannot be represented by the CSV encoder.");
        try { Utf8.GetByteCount(scalar.Value); }
        catch (EncoderFallbackException) { throw Error(node, path, "Invalid Unicode scalar value."); }
        return scalar.Value;
    }

    private IList<YamlNode> Sequence(YamlNode node, string path)
    { var sequence = node as YamlSequenceNode; if (sequence == null) throw Error(node, path, "Expected a sequence."); return sequence.Children; }

    private int Integer(YamlNode node, string path, int min, int max)
    {
        long value;
        if (!long.TryParse(Text(node, path), NumberStyles.None, Invariant, out value) || value < min || value > max)
            throw Error(node, path, "Expected an integer in " + min + ".." + max + ".");
        return (int)value;
    }

    private int Boolean(YamlNode node, string path)
    { string value = Text(node, path); if (value != "true" && value != "false") throw Error(node, path, "Expected true or false."); return value == "true" ? 1 : 0; }

    private void Ascii(string value, int max, YamlNode node, string path, bool empty = false)
    { if ((!empty && value.Length == 0) || value.Length > max || value.Any(c => c < 32 || c > 126)) throw Error(node, path, "Expected " + (empty ? "0" : "1") + ".." + max + " printable ASCII bytes."); }

    private uint Address(YamlNode node, string path)
    {
        string value = Text(node, path);
        uint address;
        if (!Regex.IsMatch(value, "^0x[0-9A-Fa-f]{6}$") || !uint.TryParse(value.Substring(2), NumberStyles.HexNumber, Invariant, out address))
            throw Error(node, path, "Expected a 24-bit address formatted as 0xAABBCC.");
        return address;
    }

    private void Load()
    {
        byte[] bytes;
        using (FileStream input = File.OpenRead(file))
        {
            if (input.Length < 1 || input.Length > 16 * 1024 * 1024) throw new InvalidDataException(file + ": YAML exceeds the 16 MiB input limit.");
            bytes = new byte[(int)input.Length]; int read = 0, next;
            while (read < bytes.Length && (next = input.Read(bytes, read, bytes.Length - read)) > 0) read += next;
            if (read != bytes.Length || input.ReadByte() != -1) throw new EndOfStreamException(file + ": YAML changed while reading.");
        }
        string text;
        try { text = Utf8.GetString(bytes); }
        catch (DecoderFallbackException exception) { throw new InvalidDataException(file + ": Invalid UTF-8.", exception); }
        if (text.Length > 0 && text[0] == '\uFEFF') text = text.Substring(1);
        sourceHash = Hash(bytes);
        var stream = new YamlStream();
        try
        {
            var parser = new Parser(new StringReader(text));
            int depth = 0;
            while (parser.MoveNext())
            {
                var nodeEvent = parser.Current as NodeEvent;
                if (parser.Current is AnchorAlias || (nodeEvent != null && (!nodeEvent.Anchor.IsEmpty || !nodeEvent.Tag.IsEmpty)))
                    throw new InvalidDataException(file + ":" + parser.Current.Start.Line + ":" + parser.Current.Start.Column + ": YAML aliases, anchors and explicit tags are not supported.");
                if (parser.Current is MappingStart || parser.Current is SequenceStart) { if (++depth > 64) throw new InvalidDataException(file + ": YAML nesting exceeds 64 levels."); }
                else if (parser.Current is MappingEnd || parser.Current is SequenceEnd) depth--;
            }
            stream.Load(new StringReader(text));
        }
        catch (YamlException exception) { throw new InvalidDataException(file + ":" + exception.Start.Line + ":" + exception.Start.Column + ": " + exception.Message, exception); }
        if (stream.Documents.Count != 1) throw new InvalidDataException(file + ": Expected exactly one YAML document.");
        YamlNode root = stream.Documents[0].RootNode;
        var top = Map(root, "$", "schemaVersion", "device", "provenance", "tree", "uiBindings", "errors");
        Integer(Required(top, "schemaVersion", root, "$"), "$.schemaVersion", 1, 1);
        ReadDevice(Required(top, "device", root, "$"));
        if (top.ContainsKey("provenance")) ReadProvenance(top["provenance"]);
        ReadTree(Required(top, "tree", root, "$"), 0, "", "$.tree", 0);
        if (parameters.Count < 1) throw Error(root, "$.tree", "At least one parameter is required.");
        if (tree.Count > 65534) throw Error(root, "$.tree", "Tree records exceed the 16-bit cursor capacity.");
        if (rangeOverrides > 64) throw Error(root, "$.tree", "More than 64 parameters tighten the runtime range.");
        if (initialBytes + parameters.Count + 64 * 8 > 128 * 1024) throw Error(root, "$.tree", "Initial buffers, states and changes already exceed the MCU's total 128 KiB SRAM; complete RAM usage still requires a firmware link.");
        if (!names.ContainsKey("FirmwareVersion")) throw Error(root, "$.tree", "The FirmwareVersion parameter is required for the application identity.");
        ReadBindings(Required(top, "uiBindings", root, "$"));
        ReadErrors(Required(top, "errors", root, "$"));
        long body = parameters.Sum(p => (long)ParameterBytes(p) + 2) + tree.Sum(r => (long)(r[0] == "1" ? 6 + Utf8.GetByteCount(r[3]) : 6) + 2)
            + bindings.Sum(b => (long)b.Role.Length + 7) + (parameters.Count + tree.Count) * 4L + errors.Sum(e => (long)e[1].Length + 4);
        if (body > 1048576 - 512) throw Error(root, "$", "Unsigned body exceeds the 1 MiB signed-catalog slot.");
    }

    private void ReadDevice(YamlNode node)
    {
        var map = Map(node, "$.device", "catalogId", "sourceVersion", "firmwareVersion", "catalogVersion", "target", "type", "name");
        catalogId = Text(Required(map, "catalogId", node, "$.device"), "$.device.catalogId");
        if (catalogId != "SW110048") throw Error(node, "$.device.catalogId", "Existing callers currently require catalogId SW110048.");
        sourceVersion = Optional(map, "sourceVersion", "$.device");
        firmwareVersion = Text(Required(map, "firmwareVersion", node, "$.device"), "$.device.firmwareVersion");
        catalogVersion = Text(Required(map, "catalogVersion", node, "$.device"), "$.device.catalogVersion");
        target = Text(Required(map, "target", node, "$.device"), "$.device.target");
        deviceName = Text(Required(map, "name", node, "$.device"), "$.device.name");
        deviceType = Integer(Required(map, "type", node, "$.device"), "$.device.type", 0, 255);
        Ascii(firmwareVersion, 31, node, "$.device.firmwareVersion"); Ascii(catalogVersion, 31, node, "$.device.catalogVersion");
        if (!Regex.IsMatch(catalogVersion, "^[A-Za-z0-9][A-Za-z0-9._-]{0,30}$")) throw Error(node, "$.device.catalogVersion", "Catalog version must be safe for a release filename.");
        Ascii(target, 16, node, "$.device.target"); Ascii(deviceName, 63, node, "$.device.name");
        if (target != "BV500-F446") throw Error(node, "$.device.target", "This firmware and host currently support only BV500-F446.");
        if (deviceName.IndexOf('"') >= 0 || deviceName.IndexOf('\\') >= 0 || firmwareVersion.IndexOf('"') >= 0 || firmwareVersion.IndexOf('\\') >= 0)
            throw Error(node, "$.device", "Quotes and backslashes are not supported in generated C identity strings.");
    }

    private void ReadProvenance(YamlNode node)
    {
        var map = Map(node, "$.provenance", "sources", "notes");
        if (map.ContainsKey("notes")) foreach (YamlNode item in Sequence(map["notes"], "$.provenance.notes")) Text(item, "$.provenance.notes[]");
        if (!map.ContainsKey("sources")) return;
        foreach (YamlNode item in Sequence(map["sources"], "$.provenance.sources"))
        {
            var source = Map(item, "$.provenance.sources[]", "path", "sha256", "origin");
            Text(Required(source, "path", item, "$.provenance.sources[]"), "$.provenance.sources[].path");
            string hash = Text(Required(source, "sha256", item, "$.provenance.sources[]"), "$.provenance.sources[].sha256");
            if (!Regex.IsMatch(hash, "^[0-9a-f]{64}$")) throw Error(item, "$.provenance.sources[].sha256", "Expected lowercase SHA-256.");
            Optional(source, "origin", "$.provenance.sources[]");
        }
    }

    private void ReadTree(YamlNode node, int parent, string parentPath, string path, int depth)
    {
        if (depth > 24) throw Error(node, path, "Tree nesting exceeds 24 group levels.");
        var children = Sequence(node, path);
        for (int i = 0; i < children.Count; i++)
        {
            YamlNode child = children[i]; string at = path + "[" + i + "]";
            var mapping = child as YamlMappingNode;
            if (mapping == null) throw Error(child, at, "Expected a group or parameter mapping.");
            if (mapping.Children.Keys.OfType<YamlScalarNode>().Any(k => k.Value == "children"))
            {
                var map = Map(child, at, "key", "title", "children", "sourcePath");
                string key = Text(Required(map, "key", child, at), at + ".key");
                if (!Regex.IsMatch(key, "^[a-z0-9_.]+$") || key.Length > 255 || !groupKeys.Add(key)) throw Error(child, at + ".key", "Invalid or duplicate ASCII group key.");
                var title = Map(Required(map, "title", child, at), at + ".title", "zh-CN", "en-US");
                string chinese = Text(Required(title, "zh-CN", child, at + ".title"), at + ".title.zh-CN");
                string english = Text(Required(title, "en-US", child, at + ".title"), at + ".title.en-US");
                if (string.IsNullOrWhiteSpace(chinese) || string.IsNullOrWhiteSpace(english) || chinese.Length > 100 || english.Length > 100) throw Error(child, at + ".title", "Translations must contain 1..100 characters.");
                if (map.ContainsKey("sourcePath")) Text(map["sourcePath"], at + ".sourcePath");
                string currentPath = parentPath.Length == 0 ? chinese : parentPath + "-" + chinese;
                int id = groups.Count + 1;
                if (id > 2300) throw Error(child, at, "The device's group-validation scratch table supports at most 2300 groups.");
                groups.Add(new Group { Id = id, Parent = parent, Key = key, Path = currentPath, Chinese = chinese, English = english });
                tree.Add(new[] { "1", id.ToString(Invariant), parent.ToString(Invariant), key, "" });
                ReadTree(Required(map, "children", child, at), id, currentPath, at + ".children", depth + 1);
            }
            else ReadParameter(child, parent, parentPath, at);
        }
    }

    private void ReadParameter(YamlNode node, int parent, string group, string path)
    {
        var map = Map(node, path, "name", "address", "type", "elementCount", "elementSize", "runtime", "description", "source");
        var p = new Parameter { Node = node, Group = group, Parent = parent };
        p.Name = Text(Required(map, "name", node, path), path + ".name"); Ascii(p.Name, 63, node, path + ".name");
        p.AddressValue = Address(Required(map, "address", node, path), path + ".address"); p.Address = "0x" + p.AddressValue.ToString("X6", Invariant);
        p.Type = Text(Required(map, "type", node, path), path + ".type"); p.TypeCode = Array.IndexOf(Types, p.Type);
        if (p.TypeCode < 0) throw Error(node, path + ".type", "Unknown type; use the canonical BVPV type names.");
        p.Count = map.ContainsKey("elementCount") ? Integer(map["elementCount"], path + ".elementCount", 1, 256) : 1;
        p.Size = p.TypeCode == 13 ? Integer(Required(map, "elementSize", node, path), path + ".elementSize", 1, 64) : Sizes[p.TypeCode];
        if (map.ContainsKey("elementSize") && Integer(map["elementSize"], path + ".elementSize", 0, 64) != p.Size) throw Error(node, path + ".elementSize", "Element size differs from its fixed type size.");
        if (p.Count * Math.Max(1, p.Size) > 3500) throw Error(node, path + ".elementCount", "Parameter buffer exceeds 3500 bytes.");
        if (!addresses.Add(p.AddressValue) || names.ContainsKey(p.Name)) throw Error(node, path, "Duplicate parameter address or name.");
        var runtime = Map(Required(map, "runtime", node, path), path + ".runtime", "access", "initial", "min", "max", "visible");
        p.RuntimeAccess = Text(Required(runtime, "access", node, path + ".runtime"), path + ".runtime.access"); AccessBits(p.RuntimeAccess, node, path + ".runtime.access");
        p.RuntimeVisible = Boolean(Required(runtime, "visible", node, path + ".runtime"), path + ".runtime.visible");
        p.RuntimeMin = Optional(runtime, "min", path + ".runtime"); p.RuntimeMax = Optional(runtime, "max", path + ".runtime");
        YamlNode initial = Required(runtime, "initial", node, path + ".runtime");
        if (p.Count == 1) p.Initial = Value(p, Text(initial, path + ".runtime.initial"), initial, path + ".runtime.initial");
        else
        {
            var values = Sequence(initial, path + ".runtime.initial");
            if (values.Count != p.Count) throw Error(initial, path + ".runtime.initial", "Initial sequence must contain elementCount values.");
            p.Initial = values.SelectMany((v, i) => Value(p, Text(v, path + ".runtime.initial[" + i + "]"), v, path + ".runtime.initial[" + i + "]")).ToArray();
        }
        var desc = Map(Required(map, "description", node, path), path + ".description", "access", "default", "min", "max", "visible", "unit", "enums", "widget", "remark", "label");
        p.Access = Optional(desc, "access", path + ".description", p.RuntimeAccess);
        AccessBits(p.Access, node, path + ".description.access");
        if (!(p.Access == p.RuntimeAccess || p.Access == "OOO" || (p.Access == "ROO" && (p.RuntimeAccess == "RWA" || p.RuntimeAccess == "RWO")) || (p.Access == "RWO" && p.RuntimeAccess == "RWA"))) throw Error(node, path + ".description.access", "Description may only restrict runtime access using the device's supported modes.");
        p.Visible = desc.ContainsKey("visible") ? Boolean(desc["visible"], path + ".description.visible") : p.RuntimeVisible;
        if (p.Visible > p.RuntimeVisible) throw Error(node, path + ".description.visible", "Description cannot reveal a runtime-hidden parameter.");
        p.Default = Optional(desc, "default", path + ".description"); p.Min = Optional(desc, "min", path + ".description", p.RuntimeMin); p.Max = Optional(desc, "max", path + ".description", p.RuntimeMax);
        p.Unit = Optional(desc, "unit", path + ".description"); p.Widget = Optional(desc, "widget", path + ".description"); p.Remark = Optional(desc, "remark", path + ".description");
        if (desc.ContainsKey("label")) ReadLabels(desc["label"], path + ".description.label");
        if (Utf8.GetByteCount(p.Group) > 4095 || Utf8.GetByteCount(p.Unit) > 255 || Utf8.GetByteCount(p.Default) > 192) throw Error(node, path, "Group, unit or default exceeds the existing encoder's field capacity.");
        if (p.Default.Length > 0) Value(p, p.Default, node, path + ".description.default");
        if (p.TypeCode == 0 && (p.Count != 1 || p.RuntimeAccess != "OOO" || p.Access != "OOO")) throw Error(node, path, "Undefined is a single, inaccessible sentinel.");
        ValidateRanges(p, node, path);
        p.Enum = desc.ContainsKey("enums") ? ReadEnums(desc["enums"], path + ".description.enums") : "";
        if (map.ContainsKey("source")) ReadParameterSource(map["source"], path + ".source");
        if (p.Name == "FirmwareVersion" && (p.Count != 1 || !p.Type.StartsWith("TEXT", StringComparison.Ordinal) || Encoding.ASCII.GetString(p.Initial).TrimEnd('\0') != firmwareVersion))
            throw Error(node, path + ".runtime.initial", "FirmwareVersion initial value must equal device.firmwareVersion.");
        if (ParameterBytes(p) > 3500) throw Error(node, path, "Parameter record exceeds 3500 bytes.");
        parameters.Add(p); names.Add(p.Name, p); initialBytes += p.Initial.Length;
        if (parameters.Count > 2300) throw Error(node, path, "The firmware supports at most 2300 parameters.");
        tree.Add(new[] { "2", "0", parent.ToString(Invariant), "", p.Address });
    }

    private void ReadLabels(YamlNode node, string path)
    { var labels = Map(node, path, "zh-CN", "en-US"); foreach (var label in labels) Text(label.Value, path + "." + label.Key); }

    private void ReadParameterSource(YamlNode node, string path)
    {
        var map = Map(node, path, "name", "dat", "ui", "compatibility");
        Optional(map, "name", path);
        if (map.ContainsKey("dat")) foreach (var field in Map(map["dat"], path + ".dat", "Name", "Address", "Type", "Default", "Min", "Max", "Access", "Remark")) Text(field.Value, path + ".dat." + field.Key);
        if (map.ContainsKey("ui"))
        {
            string[] allowed = new[] { "TreeIndexName", "CMD", "LabelName", "ReadWrite", "UIWidgetClass", "RangeFlag", "Min", "Max", "OptionNum" }.Concat(Enumerable.Range(0, 20).Select(i => "Option" + i)).ToArray();
            foreach (var field in Map(map["ui"], path + ".ui", allowed)) Text(field.Value, path + ".ui." + field.Key);
        }
        if (map.ContainsKey("compatibility")) foreach (YamlNode item in Sequence(map["compatibility"], path + ".compatibility")) Text(item, path + ".compatibility[]");
    }

    private int AccessBits(string access, YamlNode node, string path)
    { switch (access) { case "RWA": return 7; case "ROO": return 1; case "RWO": return 3; case "OWO": return 2; case "OOO": return 0; default: throw Error(node, path, "Unknown access mode."); } }

    private byte[] Value(Parameter p, string text, YamlNode node, string path)
    {
        try
        {
            if (p.TypeCode >= 10 && p.TypeCode <= 12)
            {
                Ascii(text, p.Size - 1, node, path, true); byte[] bytes = new byte[p.Size]; Encoding.ASCII.GetBytes(text).CopyTo(bytes, 0); return bytes;
            }
            if (p.TypeCode == 13)
            {
                if (!Regex.IsMatch(text, "^[0-9A-Fa-f]+$") || text.Length != p.Size * 2) throw Error(node, path, "COMPOSITE needs exactly elementSize hexadecimal bytes.");
                return Enumerable.Range(0, p.Size).Select(i => byte.Parse(text.Substring(i * 2, 2), NumberStyles.HexNumber, Invariant)).ToArray();
            }
            if (p.TypeCode == 0) { if (text != "0" && text != "") throw Error(node, path, "Undefined initial value is zero."); return new byte[1]; }
            if (text.Length == 0) text = "0";
            if (p.TypeCode == 6)
            {
                double number = double.Parse(text, NumberStyles.Float, Invariant); float value = (float)number;
                if (double.IsNaN(number) || double.IsInfinity(number) || float.IsNaN(value) || float.IsInfinity(value)) throw Error(node, path, "FLOAT32 must be finite and representable.");
                return BitConverter.GetBytes(value);
            }
            if (p.TypeCode == 7 || p.TypeCode == 8)
            {
                double number = double.Parse(text, NumberStyles.Float, Invariant);
                double rounded = Math.Truncate(number * 65536.0 + (number >= 0 ? 0.5 : -0.5));
                if (double.IsNaN(rounded) || double.IsInfinity(rounded) || (p.TypeCode == 7 ? rounded < int.MinValue || rounded > int.MaxValue : number < 0 || rounded > uint.MaxValue))
                    throw Error(node, path, "Fixed-point value cannot be represented without overflow.");
                return p.TypeCode == 7 ? BitConverter.GetBytes((int)rounded) : BitConverter.GetBytes((uint)rounded);
            }
            if (p.TypeCode == 5)
            {
                long number = long.Parse(text, NumberStyles.AllowLeadingSign, Invariant);
                if (number < int.MinValue || number > int.MaxValue) throw Error(node, path, "INT32 is outside its type range.");
                return BitConverter.GetBytes((int)number);
            }
            ulong unsigned;
            if (text.StartsWith("0x", StringComparison.Ordinal)) unsigned = ulong.Parse(text.Substring(2), NumberStyles.HexNumber, Invariant);
            else unsigned = ulong.Parse(text, NumberStyles.None, Invariant);
            ulong limit = p.Size == 8 ? ulong.MaxValue : (1UL << (p.Size * 8)) - 1;
            if (unsigned > limit) throw Error(node, path, "Unsigned value is outside its type range.");
            return Enumerable.Range(0, p.Size).Select(i => (byte)(unsigned >> (i * 8))).ToArray();
        }
        catch (FormatException) { throw Error(node, path, "Value does not match its declared type."); }
        catch (OverflowException) { throw Error(node, path, "Value overflows its declared type."); }
    }

    private int Compare(Parameter p, string first, string second, YamlNode node, string path)
    {
        byte[] a = Value(p, first, node, path), b = Value(p, second, node, path);
        if (p.TypeCode == 6) return BitConverter.ToSingle(a, 0).CompareTo(BitConverter.ToSingle(b, 0));
        if (p.TypeCode == 5 || p.TypeCode == 7) return BitConverter.ToInt32(a, 0).CompareTo(BitConverter.ToInt32(b, 0));
        ulong x = 0, y = 0; for (int i = a.Length - 1; i >= 0; i--) { x = x << 8 | a[i]; y = y << 8 | b[i]; } return x.CompareTo(y);
    }

    // C 的整数解析使用 base=0；导出规范十进制，避免 YAML 中前导零变成八进制。
    private string EncoderValue(Parameter p, string text)
    {
        if (text.Length == 0 || p.TypeCode >= 10 || p.TypeCode == 0) return text;
        byte[] bytes = Value(p, text, p.Node, p.Name);
        if (p.TypeCode == 6) return BitConverter.ToSingle(bytes, 0).ToString("R", Invariant);
        if (p.TypeCode == 5) return BitConverter.ToInt32(bytes, 0).ToString(Invariant);
        if (p.TypeCode == 7) return (BitConverter.ToInt32(bytes, 0) / 65536.0).ToString("R", Invariant);
        if (p.TypeCode == 8) return (BitConverter.ToUInt32(bytes, 0) / 65536.0).ToString("R", Invariant);
        ulong value = 0; for (int i = bytes.Length - 1; i >= 0; i--) value = value << 8 | bytes[i]; return value.ToString(Invariant);
    }

    private void ValidateRanges(Parameter p, YamlNode node, string path)
    {
        if (p.TypeCode == 0 || p.TypeCode >= 10)
        { if (p.Min.Length + p.Max.Length + p.RuntimeMin.Length + p.RuntimeMax.Length > 0) throw Error(node, path, "Only numeric types can have bounds."); return; }
        Func<string, string, int> compare = (a, b) => Compare(p, a, b, node, path);
        if (p.RuntimeMin.Length > 0 && p.RuntimeMax.Length > 0 && compare(p.RuntimeMin, p.RuntimeMax) > 0) throw Error(node, path + ".runtime", "Minimum exceeds maximum.");
        if (p.Min.Length > 0 && p.Max.Length > 0 && compare(p.Min, p.Max) > 0) throw Error(node, path + ".description", "Minimum exceeds maximum.");
        if (p.RuntimeMin.Length > 0 && (p.Min.Length == 0 || compare(p.Min, p.RuntimeMin) < 0)) throw Error(node, path + ".description.min", "Description must retain or tighten the hard minimum.");
        if (p.RuntimeMax.Length > 0 && (p.Max.Length == 0 || compare(p.Max, p.RuntimeMax) > 0)) throw Error(node, path + ".description.max", "Description must retain or tighten the hard maximum.");
        foreach (string bound in new[] { p.RuntimeMin, p.RuntimeMax, p.Min, p.Max }) if (bound.Length > 0) Value(p, bound, node, path);
        Func<string, byte[]> encoded = s => s.Length == 0 ? null : Value(p, s, node, path);
        if (!Equal(encoded(p.Min), encoded(p.RuntimeMin)) || !Equal(encoded(p.Max), encoded(p.RuntimeMax))) rangeOverrides++;
    }

    private string ReadEnums(YamlNode node, string path)
    {
        var items = Sequence(node, path);
        if (items.Count > 32) throw Error(node, path, "At most 32 enum entries are supported.");
        var seen = new HashSet<uint>(); var result = new List<string>();
        foreach (YamlNode item in items)
        {
            var map = Map(item, path + "[]", "value", "text");
            YamlNode valueNode = Required(map, "value", item, path + "[]"); string value = Text(valueNode, path + "[].value"); long number;
            if (!long.TryParse(value, NumberStyles.AllowLeadingSign, Invariant, out number) || number < int.MinValue || number > uint.MaxValue || !seen.Add(unchecked((uint)number))) throw Error(valueNode, path + "[].value", "Invalid or duplicate 32-bit enum value.");
            string text = Text(Required(map, "text", item, path + "[]"), path + "[].text");
            if (Utf8.GetByteCount(text) > 255 || text.IndexOf(';') >= 0) throw Error(item, path + "[].text", "Enum text exceeds 255 UTF-8 bytes or contains the CSV enum separator ';'.");
            result.Add(number.ToString(Invariant) + "=" + text);
        }
        return string.Join(";", result);
    }

    private int ParameterBytes(Parameter p)
    {
        int bytes = 12 + p.Name.Length;
        if (p.Min.Length > 0) bytes += 3 + p.Size;
        if (p.Max.Length > 0) bytes += 3 + p.Size;
        if (p.Default.Length > 0) bytes += 3 + p.Size;
        if (p.Unit.Length > 0) bytes += 3 + Utf8.GetByteCount(p.Unit);
        if (!string.IsNullOrEmpty(p.Enum)) bytes += 3 + p.Enum.Split(';').Sum(e => 5 + Utf8.GetByteCount(e.Substring(e.IndexOf('=') + 1)));
        return bytes;
    }

    private void ReadBindings(YamlNode node)
    {
        var items = Sequence(node, "$.uiBindings"); var roles = new HashSet<string>(StringComparer.Ordinal); var orders = new HashSet<string>(StringComparer.Ordinal);
        if (items.Count > 64) throw Error(node, "$.uiBindings", "At most 64 UI bindings are supported.");
        foreach (YamlNode item in items)
        {
            var map = Map(item, "$.uiBindings[]", "role", "parameter", "order");
            string role = Text(Required(map, "role", item, "$.uiBindings[]"), "$.uiBindings[].role");
            string parameter = Text(Required(map, "parameter", item, "$.uiBindings[]"), "$.uiBindings[].parameter");
            int order = Integer(Required(map, "order", item, "$.uiBindings[]"), "$.uiBindings[].order", 0, 255);
            if (!Roles.Contains(role) || !roles.Add(role)) throw Error(item, "$.uiBindings[].role", "Unsupported or duplicate built-in UI role.");
            Parameter p; if (!names.TryGetValue(parameter, out p)) throw Error(item, "$.uiBindings[].parameter", "Unknown parameter reference.");
            if (!orders.Add(role.Split('.')[0] + "." + order)) throw Error(item, "$.uiBindings[].order", "Duplicate order within a UI area.");
            bindings.Add(new Binding { Role = role, Address = p.Address, Order = order });
        }
    }

    private void ReadErrors(YamlNode node)
    {
        var items = Sequence(node, "$.errors"); var seen = new HashSet<int>();
        if (items.Count < 1 || items.Count > 256) throw Error(node, "$.errors", "Expected 1..256 error texts.");
        foreach (YamlNode item in items)
        {
            var map = Map(item, "$.errors[]", "code", "text", "symbol");
            string text = Text(Required(map, "text", item, "$.errors[]"), "$.errors[].text"); Ascii(text, 255, item, "$.errors[].text");
            YamlNode codeNode = Required(map, "code", item, "$.errors[]"); string code = Text(codeNode, "$.errors[].code"); int value;
            if (!Regex.IsMatch(code, "^0x[0-9A-Fa-f]{2}$") || !int.TryParse(code.Substring(2), NumberStyles.HexNumber, Invariant, out value) || !seen.Add(value)) throw Error(codeNode, "$.errors[].code", "Expected a unique code formatted as 0xNN.");
            Optional(map, "symbol", "$.errors[]"); errors.Add(new[] { "0x" + value.ToString("X2", Invariant), text });
        }
    }

    private void Export(string destination)
    {
        var files = new Dictionary<string, string>(StringComparer.Ordinal);
        string[] header = { "Group", "Name", "Address", "Type", "Access", "Default", "Min", "Max", "Unit", "Enum", "Visible", "Widget", "Remark", "RuntimeAccess", "RuntimeMin", "RuntimeMax", "RuntimeVisible", "InitialBytes", "ElementCount", "ElementSize", "DeviceType", "DeviceName" };
        files.Add(catalogId + ".catalog.csv", Csv(header, parameters.Select(p => new[] { p.Group, p.Name, p.Address, p.Type, p.Access, EncoderValue(p, p.Default), EncoderValue(p, p.Min), EncoderValue(p, p.Max), p.Unit, p.Enum, p.Visible.ToString(Invariant), p.Widget, p.Remark, p.RuntimeAccess, EncoderValue(p, p.RuntimeMin), EncoderValue(p, p.RuntimeMax), p.RuntimeVisible.ToString(Invariant), Hex(p.Initial), p.Count.ToString(Invariant), p.Size.ToString(Invariant), deviceType.ToString(Invariant), deviceName })));
        files.Add(catalogId + ".tree.csv", Csv(new[] { "Kind", "NodeId", "ParentId", "Key", "Address" }, tree));
        files.Add("ui-bindings.csv", Csv(new[] { "Role", "Address", "Order" }, bindings.Select(b => new[] { b.Role, b.Address, b.Order.ToString(Invariant) })));
        files.Add("errors.csv", Csv(new[] { "Code", "Text" }, errors));
        var json = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 };
        files.Add("groups.v2.json", json.Serialize(new { schemaVersion = 2, groups = groups.Select(g => new Dictionary<string, object> { { "sourcePath", g.Path }, { "key", g.Key }, { "zh-CN", g.Chinese }, { "en-US", g.English } }).ToArray() }) + "\n");
        byte[] contract;
        using (var memory = new MemoryStream())
        using (var writer = new BinaryWriter(memory, Encoding.ASCII, true))
        {
            writer.Write(Encoding.ASCII.GetBytes("BVPV-RUNTIME-1")); writer.Write((ushort)parameters.Count);
            foreach (Parameter p in parameters.OrderBy(p => p.AddressValue)) { writer.Write((byte)(p.AddressValue >> 16)); writer.Write((byte)(p.AddressValue >> 8)); writer.Write((byte)p.AddressValue); writer.Write((byte)p.TypeCode); writer.Write((ushort)p.Count); writer.Write((ushort)p.Size); }
            writer.Flush(); contract = memory.ToArray();
        }
        byte[] rules;
        using (var memory = new MemoryStream())
        using (var writer = new BinaryWriter(memory, Encoding.ASCII, true))
        {
            foreach (Parameter p in parameters.OrderBy(p => p.AddressValue))
            {
                writer.Write(p.AddressValue); writer.Write(p.RuntimeAccess); writer.Write((byte)p.RuntimeVisible);
                foreach (string bound in new[] { p.RuntimeMin, p.RuntimeMax }) { writer.Write(bound.Length != 0); if (bound.Length != 0) writer.Write(Value(p, bound, p.Node, p.Name + ".runtime")); }
            }
            writer.Flush(); rules = memory.ToArray();
        }
        files.Add("source-info.json", json.Serialize(new { schemaVersion = 1, catalogId, sourceVersion, firmwareVersion, device = new { target, name = deviceName, type = deviceType, catalogVersion }, sourceSha256 = sourceHash, parameterCount = parameters.Count, groupCount = groups.Count, treeRecordCount = tree.Count, uiCount = bindings.Count, errorCount = errors.Count, rangeOverrideCount = rangeOverrides, initialByteCount = initialBytes, runtimeContractSha256 = Hash(contract), runtimeRulesSha256 = Hash(rules), initialValueSha256 = Hash(parameters.OrderBy(p => p.AddressValue).SelectMany(p => p.Initial).ToArray()), bindingsSha256 = Hash(Utf8.GetBytes(files["ui-bindings.csv"])) }) + "\n");
        string root = Path.GetFullPath(destination);
        foreach (string name in files.Keys) if (File.Exists(Path.Combine(root, name))) throw new IOException("Source output already exists: " + Path.Combine(root, name));
        Directory.CreateDirectory(root);
        foreach (var output in files) File.WriteAllText(Path.Combine(root, output.Key), output.Value, Utf8);
    }

    private static string Csv(string[] header, IEnumerable<string[]> rows)
    { Func<string[], string> line = values => string.Join(",", values.Select(v => "\"" + v.Replace("\"", "\"\"") + "\"")); return line(header) + "\n" + string.Join("", rows.Select(r => line(r) + "\n")); }
    private static string Hex(byte[] value) { return BitConverter.ToString(value).Replace("-", ""); }
    private static string Hash(byte[] value) { using (SHA256 hash = SHA256.Create()) return Hex(hash.ComputeHash(value)).ToLowerInvariant(); }
    private static bool Equal(byte[] first, byte[] second) { return first == null ? second == null : second != null && first.SequenceEqual(second); }
}
