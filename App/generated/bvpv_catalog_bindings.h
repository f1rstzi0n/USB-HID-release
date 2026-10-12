/* 自动生成自 catalog/SW110048.catalog.yaml，请勿手改。 */
#ifndef BVPV_CATALOG_BINDINGS_H
#define BVPV_CATALOG_BINDINGS_H
#include <stdint.h>
#define BVPV_CATALOG_BINDING_COUNT 29u
static const struct { const char *key; uint32_t addr; } bvpv_catalog_bindings[] = {
    { "status.product", 0x010107u },
    { "status.access-mode", 0x7001D1u },
    { "status.control-mode", 0x690103u },
    { "status.controller", 0x7101C0u },
    { "status.error-number", 0x6B01F6u },
    { "status.error-code", 0x6B01F7u },
    { "status.firmware", 0x640105u },
    { "button.open", 0x690103u },
    { "button.close", 0x690103u },
    { "button.learn", 0x7001C9u },
    { "button.zero-adjust", 0x6801BAu },
    { "button.restart", 0x030103u },
    { "led.open", 0x690103u },
    { "led.closed", 0x690103u },
    { "position.feedback", 0x6A0191u },
    { "position.target", 0x6A0190u },
    { "pressure.feedback", 0x710106u },
    { "pressure.target", 0x7001C8u },
    { "chart.position.feedback", 0x6A0191u },
    { "chart.position.target", 0x6A0190u },
    { "chart.pressure.feedback", 0x710106u },
    { "chart.pressure.target", 0x7001C8u },
    { "info.product-code", 0x010103u },
    { "info.device-type", 0x640101u },
    { "info.communication-address", 0x030101u },
    { "info.baud-rate", 0x030102u },
    { "info.control-mode", 0x690103u },
    { "info.function-mode", 0x6A0101u },
    { "info.alarm", 0x6501A0u },
};
#endif
