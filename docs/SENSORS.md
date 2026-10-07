# Board thermal sensors (GB10)

The **Board** panel (`board_acpi.so`) shows a title bar labeled **Board**, then seven ACPI thermal zones exposed by the DGX Spark / GB10 firmware under `/sys/class/thermal/`. There is **no Fan row**: chassis fans are driven by the Embedded Controller (EC), and Linux typically has no usable RPM/PWM for them on this platform.

Values are millidegrees Celsius in sysfs; the panel displays whole °C.

## How zones are discovered

1. Scan `thermal_zone0` … `thermal_zoneN`.  
2. Read `temp` (millidegree C).  
3. Resolve the ACPI short name via `device` → ACPI `path` (last path component), e.g. `TSOC`.  
4. Match names in the fixed display order below.

Kernel `type` is usually the generic `acpitz`; **names** come from ACPI, not from that type string.

## Sensor reference

| Label | Typical zone* | What it represents | Why it matters |
|-------|---------------|--------------------|----------------|
| **TSOC** | often `thermal_zone0` | **SoC / package** platform sensor for the Grace+Blackwell module | Frequently the hottest under sustained inference; correlates with package throttling more than NVML GPU die temp alone |
| **TGPU** | often mid/high index | **GPU-domain ACPI** sensor (firmware zone named `TGPU`) | Complements NVML “GPU Temp” in the nvidia panel; the two can diverge under UMA / SoC load |
| **TS0E** | — | Thermal sensor **bank 0, edge** (`E` = edge) | Board/skin-side reading on sensor group 0; usually cooler than package points |
| **TS0P** | — | Thermal sensor **bank 0, proximity / package point** (`P`) | Near a hot component on bank 0; useful for spotting localized heating |
| **TS1E** | — | Thermal sensor **bank 1, edge** | Same role as TS0E on the second sensor bank (other side / other rail of the module) |
| **TS1P** | — | Thermal sensor **bank 1, proximity / package point** | Same role as TS0P on bank 1 |
| **TUNC** | often last zone | **Uncore / auxiliary** platform sensor (`UNC`) | Extra chassis/module reading; useful as a third opinion when TSOC and TGPU disagree |

\*Exact `thermal_zoneN` indices can differ by firmware; the dock always keys off the **ACPI name**, not the zone number.

### Naming convention (GB10 ACPI)

```text
T    = thermal zone
SOC  = system-on-chip package
GPU  = GPU domain (ACPI, not NVML)
S0/S1 = sensor bank 0 / 1
E    = edge
P    = proximity / package point
UNC  = uncore / auxiliary
```

NVIDIA has not published a full public datasheet mapping every zone to a physical thermistor. The table above follows the ACPI short names, observed dmesg registration (`Thermal Zone [TSOC]`, `[TGPU]`, …), and community measurements on DGX Spark FE/OEM units. Treat **TSOC** (and often **TGPU**) as the primary “is the box cooking?” signals for long inference jobs.

## Fan (not shown)

| Topic | Detail |
|-------|--------|
| Why removed | OS rarely exposes RPM; the Board row was stuck on `—` and wasted vertical space |
| Who controls fans | Embedded Controller firmware (fan curve), not GKrellM / `fancontrol` |
| GPU fan in NVML | GB10 typically has no useful NVML fan either; those decals stay off in managed config |

## Related readouts elsewhere in the dock

| Place | Sensor | Relation to Board |
|-------|--------|-------------------|
| `nvidia` text row | NVML GPU temperature | Die/sensor from the driver; compare with **TGPU** / **TSOC** |
| `nvidia` text row | UMA % | Memory pressure, not temperature |
| `uma_dram` (optional, off by default) | Used DRAM | Load that often drives TSOC up even when GPU util looks modest |

## Operational tips

- Under long NIM/vLLM runs, watch **TSOC** (and **TGPU**) in Board, not only NVML Temp.  
- If Board shows `—` for a name, that ACPI zone is missing from sysfs on this firmware image.  
- Re-read live values: `cat /sys/class/thermal/thermal_zone*/temp` and match names via ACPI `path` as the plugin does.
