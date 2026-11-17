/* SPDX-License-Identifier: Apache-2.0 */
/* Provenance: original a4l file; declarations from the capget(2) manual page.
 * No GPL-3.0 sources. */
/* a4l: minimal <sys/capability.h> for sysroots without libcap's headers. glibc
 * exports capget and capset; the structures come from the kernel UAPI header.
 */
#ifndef A4L_SYS_CAPABILITY_H
#define A4L_SYS_CAPABILITY_H

#include <linux/capability.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int capget(cap_user_header_t header, cap_user_data_t data);
int capset(cap_user_header_t header, const cap_user_data_t data);

#ifdef __cplusplus
}
#endif

#endif
