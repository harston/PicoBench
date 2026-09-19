/*
 * USB CDC console, available together with the report disk after the run.
 *   d  print REPORT.TXT of the latest run     e  print ERRORS.TXT
 *   l  list stored runs                       r  run the tests again (reboots)
 *   b  reboot into BOOTSEL (UF2 flashing)     h  help
 */

#ifndef PICOBENCH_CONSOLE_H
#define PICOBENCH_CONSOLE_H

void console_task(void);

#endif
