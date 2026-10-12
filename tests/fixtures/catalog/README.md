# External catalog test fixtures

The synthetic two-parameter ACAT v1 packages use an ephemeral P-256 key generated only in memory. public-key.bin contains only the 64-byte public point. No private key is stored or suitable for production. Valid records include lengthy enumeration descriptions to cross page and sector boundaries. Rejected variants have valid signatures unless their names explicitly say bad-signature or bad-digest.

The range-overrides fixtures use a separate public key and 65 synthetic parameters to test the 64-override limit. The real-catalog package and public key are generated from the current YAML into the host build directory with an ephemeral CNG self-test key. All signatures cover SHA256("BVPV-CATALOG-V1" plus NUL plus header bytes 0..447). Default values remain descriptive: historical initialization bytes may lie outside the active write range. The bad-default fixture carries an incorrect Default TLV length.
