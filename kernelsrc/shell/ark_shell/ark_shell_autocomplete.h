/* ============================================================
 * ark_shell_autocomplete.h - Tab Autocomplete Declaration
 *
 * Prefix matching, common prefix completion, candidate listing.
 * ============================================================ */
#ifndef ARK_SHELL_AUTOCOMPLETE_H
#define ARK_SHELL_AUTOCOMPLETE_H

#include "ark_shell_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ark_shell_s;  /* Forward declaration */

/* Perform Tab completion. Pass shell main struct pointer */
void ark_shell_autocomplete(struct ark_shell_s *shell);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_AUTOCOMPLETE_H */
