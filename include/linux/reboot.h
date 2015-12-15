#ifndef _LINUX_REBOOT_H
#define _LINUX_REBOOT_H


#include <linux/notifier.h>
#include <uapi/linux/reboot.h>

#define SYS_DOWN	0x0001	/* Notify of system down */
#define SYS_RESTART	SYS_DOWN
#define SYS_HALT	0x0002	/* Notify of system halt */
#define SYS_POWER_OFF	0x0003	/* Notify of system power off */

enum reboot_mode {
	REBOOT_COLD = 0,
	REBOOT_WARM,
	REBOOT_HARD,
	REBOOT_SOFT,
	REBOOT_GPIO,
};

extern int register_reboot_notifier(struct notifier_block *);
extern int unregister_reboot_notifier(struct notifier_block *);


/*
 * Architecture-specific implementations of sys_reboot commands.
 */

extern void machine_restart(char *cmd) __noreturn;
extern void machine_halt(void) __noreturn;
extern void machine_power_off(void) __noreturn;


extern void machine_shutdown(void);
struct pt_regs;
extern void machine_crash_shutdown(struct pt_regs *);

/* 
 * Architecture independent implemenations of sys_reboot commands.
 */

extern void kernel_restart_prepare(char *cmd);
extern void kernel_restart(char *cmd) __noreturn;
extern void kernel_halt(void) __noreturn;
extern void kernel_power_off(void) __noreturn;

extern int C_A_D; /* for sysctl */
void ctrl_alt_del(void);

#define POWEROFF_CMD_PATH_LEN	256
extern char poweroff_cmd[POWEROFF_CMD_PATH_LEN];

extern int orderly_poweroff(bool force);

/*
 * Emergency restart, callable from an interrupt handler.
 */

extern void emergency_restart(void) __noreturn;
#include <asm/emergency-restart.h>

#endif /* _LINUX_REBOOT_H */
