# SPDX-License-Identifier: GPL-2.0-or-later
"""Environment policy for physical-lever development launchers.

Normal test sessions respect game permissions and dispatch real activation.
Motion-only rehearsal must be explicitly requested; it bypasses permissions and
never dispatches gameplay activation. Ordinary VR launches also install the physical adapter.
"""


def configure_lever_test_mode(environment: dict[str, str], *, motion_only: bool = False) -> None:
    """Overwrite inherited mode flags so a prior rehearsal cannot leak into a test."""
    environment["VR_LEVER_PHYSICAL_TEST"] = "0" if motion_only else "1"
    environment["VR_LEVER_MOTION_TEST"] = "1" if motion_only else "0"
