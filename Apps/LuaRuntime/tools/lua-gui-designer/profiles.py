"""Device profiles for the PC Lua GUI designer.

The values come from the firmware simulator profiles
(`Devices/simulator/Source/SimulatorProfile.cpp`) so the preview uses the same
display size, DPI, default font size and appearance defaults as a simulated
device. Profiles with a landscape and portrait variant list both; the firmware
profile's `portrait` flag only says which rotation it starts in.

The designer edits the content area below the status bar and the app toolbar.
That area's size and inset are derived from the theme settings and DPI in
`theme.py`, not stored here, because the appearance settings change them.
"""

from __future__ import annotations

from dataclasses import dataclass

from theme import ThemeSettings


@dataclass(frozen=True)
class DeviceProfile:
    id: str
    name: str
    width: int
    height: int
    dpi: int
    font_size: int
    spacing: int
    size: int
    dark: bool = True
    mono: bool = False
    touch: bool = True

    def default_theme(self) -> ThemeSettings:
        return ThemeSettings(
            dark=False if self.mono else self.dark,
            mono=self.mono,
            spacing=self.spacing,
            size=self.size,
            font_size=self.font_size,
        )


_TAB5 = dict(dpi=294, font_size=29, spacing=100, size=100)

PROFILES: tuple[DeviceProfile, ...] = (
    DeviceProfile(
        id="tab5-landscape",
        name="M5Stack Tab5 landscape (1280x720)",
        width=1280,
        height=720,
        **_TAB5,
    ),
    DeviceProfile(
        id="tab5-portrait",
        name="M5Stack Tab5 portrait (720x1280)",
        width=720,
        height=1280,
        **_TAB5,
    ),
    DeviceProfile(
        id="mypuler-landscape",
        name="Sixrig myPuter (928x272)",
        width=928,
        height=272,
        dpi=250,
        font_size=24,
        spacing=40,
        size=60,
    ),
    DeviceProfile(
        id="mypuler-portrait",
        name="Sixrig myPuter portrait (272x928)",
        width=272,
        height=928,
        dpi=250,
        font_size=24,
        spacing=40,
        size=60,
    ),
    DeviceProfile(
        id="default-640x320",
        name="Default (640x320)",
        width=640,
        height=320,
        dpi=130,
        font_size=14,
        spacing=100,
        size=100,
    ),
    DeviceProfile(
        id="lilygo-tdeck",
        name="LilyGO T-Deck (320x240)",
        width=320,
        height=240,
        dpi=143,
        font_size=14,
        spacing=40,
        size=60,
    ),
    DeviceProfile(
        id="lilygo-tlora-pager",
        name="LilyGO T-Lora Pager (480x222)",
        width=480,
        height=222,
        dpi=227,
        font_size=16,
        spacing=40,
        size=0,
        touch=False,
    ),
    DeviceProfile(
        id="m5stack-cardputer",
        name="M5Stack Cardputer (240x135)",
        width=240,
        height=135,
        dpi=139,
        font_size=14,
        spacing=20,
        size=0,
        touch=False,
    ),
    DeviceProfile(
        id="cl-32",
        name="CL-32 (384x168)",
        width=384,
        height=168,
        dpi=139,
        font_size=14,
        spacing=20,
        size=0,
        mono=True,
        touch=False,
    ),
)


def find_profile(profile_id: str) -> DeviceProfile:
    for profile in PROFILES:
        if profile.id == profile_id:
            return profile
    return PROFILES[0]
