RCX Hashtables
==============

This directory contains source code for hash table implementations protected by
RCX, RCU, RLU, RCU-HTM, and few more alternatives and a program for evaluation
of the performance of the tables.

Please note that this is a fork of the RLU (https://github.com/rlu-sync/rlu)
and considerably modified, though the code for RLU-protected hash tables are
unmodified.


Omitted RCU Critical Sections in EuroSys'20 Paper
=================================================

The code snippet for `rcx_list_[add|remove]()` in the EuroSys'20 paper is
omitting the `rcu_read_[un]lock()` calls for brevity, as the code is assumed to
run on a machine that provides the total store order and runs the
non-preemptible kernel for brevity and thus `rcu_read_[un]lock()` becomes
no-op.  Sorry if this made you confused.
