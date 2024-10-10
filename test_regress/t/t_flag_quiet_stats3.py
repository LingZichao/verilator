#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2024 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')
test.top_filename = "t/t_flag_quiet_stats.v"

test.compile(verilator_flags2=['--quiet --no-quiet-stats'],
             verilator_make_gcc=False,
             logfile=test.run_log_filename)

test.file_grep(test.compile_log_filename, r'V e r i l a t')

test.passes()