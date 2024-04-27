# SPDX-FileCopyrightText: 2023 Ondřej Surý
#
# SPDX-License-Identifier: WTFPL

project('list-benchmark', 'c', default_options : ['c_std=gnu17'])

thread_dep = dependency('threads')
libuv_dep = dependency('libuv')
urcu_dep = dependency('liburcu-memb')
urcu_cds_dep = dependency('liburcu-cds')

executable('bench', ['list-bench.c'],
           dependencies : [
             thread_dep,
             libuv_dep,
             urcu_dep,
             urcu_cds_dep,
           ],
          )
