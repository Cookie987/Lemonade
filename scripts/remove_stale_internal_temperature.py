Import("env")  # noqa: F821

from pathlib import Path


def remove_stale_internal_temperature(env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))

    component_dir = project_dir / "src" / "esphome" / "components" / "internal_temperature"
    legacy_cpp = component_dir / "internal_temperature.cpp"
    split_files = [
        component_dir / "internal_temperature_common.cpp",
        component_dir / "internal_temperature_esp32.cpp",
        component_dir / "internal_temperature_rp2040.cpp",
        component_dir / "internal_temperature_nrf52.cpp",
    ]

    if not legacy_cpp.exists():
        return

    if not any(path.exists() for path in split_files):
        return

    print(f"Removing stale ESPHome source: {legacy_cpp}")
    legacy_cpp.unlink()

    stale_outputs = [
        build_dir
        / "src"
        / "esphome"
        / "components"
        / "internal_temperature"
        / "internal_temperature.cpp.o",
        build_dir
        / "src"
        / "esphome"
        / "components"
        / "internal_temperature"
        / "internal_temperature.cpp.o.d",
    ]
    for path in stale_outputs:
        if path.exists():
            print(f"Removing stale ESPHome object: {path}")
            path.unlink()


remove_stale_internal_temperature(env)
