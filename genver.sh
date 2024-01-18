#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Script to generate the module version header
#

if command -v git >/dev/null 2>&1 && [ -d .git ]; then
	GIT_COMMIT=$(git describe HEAD | tail -c 9)
else
	GIT_COMMIT="$(grep PACKAGE_VERSION dkms.conf | cut -d '"' -f2)?"
fi

printf '#define GIT_COMMIT\t"%s"\n' "$GIT_COMMIT" > version.h
