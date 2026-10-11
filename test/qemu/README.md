# QEMU end-to-end test

Runs the firmware in Espressif QEMU against two fake NTRIP casters on the host.

QEMU has no WiFi, so this uses a test build (`CONFIG_NTRIP_QEMU_TEST`) that brings up the emulated
OpenCores Ethernet in place of WiFi and disables the GPIO0 reset button. Never flash that build to a board.

```sh
. $IDF_PATH/export.sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # needs libslirp0 on Linux
idf.py -B build_qemu -D SDKCONFIG=build_qemu/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig;test/qemu/sdkconfig.qemu" build
python test/qemu/run_qemu_test.py build_qemu
```

The script feeds framed, sequence-numbered data into UART0 and checks (about 6 minutes):

- both casters log in and receive clean, in-order data
- caster A reconnects about 5 s after a caster-side drop
- refused logins back off with growing gaps
- caster B keeps streaming at full rate while caster A stops reading for 150 s
- no panic or reboot

Limitation: slirp buffers without limit, so a caster that stops reading never pushes back on the ESP32's
TCP stack. `test/host` covers back-pressure with real kernel sockets.
