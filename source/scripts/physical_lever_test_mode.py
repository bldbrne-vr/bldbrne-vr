# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
"""Environment policy for physical-lever development launchers.

Normal test sessions respect game permissions and dispatch real activation.
Motion-only rehearsal must be explicitly requested; it bypasses permissions and
never dispatches gameplay activation. Ordinary VR launches also install the physical adapter.
"""


def configure_lever_test_mode(environment: dict[str, str], *, motion_only: bool = False) -> None:
    """Overwrite inherited mode flags so a prior rehearsal cannot leak into a test."""
    environment["VR_LEVER_PHYSICAL_TEST"] = "0" if motion_only else "1"
    environment["VR_LEVER_MOTION_TEST"] = "1" if motion_only else "0"
