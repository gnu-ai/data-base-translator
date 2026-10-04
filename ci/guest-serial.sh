#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# The serial stub of the GNU/Hurd CI (fed to the guest through
# the serial console by hurd_vm.py).
#
# The serial console has no flow control: a script fed line by
# line through it loses characters once it reaches a few
# kilobytes.  So the console only carries THIS stub, four lines
# long; the real guest script (ci/guest-hurd.sh of the
# repository) is served by the CI host over plain HTTP on the
# QEMU gateway (10.0.2.2:8000, see .github/workflows/hurd.yml)
# and executed here.  The exit code of the real script becomes
# the exit code of this one - and of the driver.

set -e

command -v wget >/dev/null 2>&1 || {
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq wget
}

wget -q -O /tmp/real.sh http://10.0.2.2:8000/guest-hurd.sh \
    || { echo "FAIL: the CI host is not serving guest-hurd.sh on :8000"; exit 1; }

exec sh /tmp/real.sh
