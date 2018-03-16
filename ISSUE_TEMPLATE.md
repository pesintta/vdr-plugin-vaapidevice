Describe what issue you are experiencing. Attach any information that could be helpful in debugging the issue (screen shots, log messages etc).

Please attach output of generate_system_report.sh skript when reporting new issues.
```
export DISPLAY=:0.0
./generate_system_report.sh
```

If you're reporting a crash in vaapidevice (e.g. a segmentation fault) then please attach backtrace (command `bt`) from gdb.

Sections 1-4 of the following tutorial can be helpful in first generating a meaningful core dump and then printing a backtrace from it.
http://www.brendangregg.com/blog/2016-08-09/gdb-example-ncurses.html
