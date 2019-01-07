
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "vterm.h"
#include "vterm_private.h"
#include "vterm_csi.h"
#include "vterm_osc.h"
#include "vterm_render.h"
#include "vterm_misc.h"
#include "vterm_buffer.h"
#include "macros.h"

static int
vterm_interpret_esc_normal(vterm_t *vterm);

static int
vterm_interpret_esc_xterm_dsc(vterm_t *vterm);

static int
vterm_interpret_esc_scs(vterm_t *vterm);

inline bool
validate_csi_escape_suffix(char *lastchar);

inline bool
validate_xterm_escape_suffix(char *lastcharc);

inline bool
validate_scs_escape_suffix(char *lastchar);

void
vterm_escape_start(vterm_t *vterm)
{
    vterm->internal_state |= STATE_ESCAPE_MODE;

    // zero out the escape buffer just in case
    vterm->esbuf_len = 0;
    vterm->esbuf[0] = '\0';

    vterm->esc_handler = NULL;

    return;
}

void
vterm_escape_cancel(vterm_t *vterm)
{
    vterm->internal_state &= ~STATE_ESCAPE_MODE;

    // zero out the escape buffer for the next run
    vterm->esbuf_len = 0;
    vterm->esbuf[0] = '\0';

    vterm->esc_handler = NULL;

    return;
}

void
vterm_interpret_escapes(vterm_t *vterm)
{
    static char         interims[] = "[]P()";
    static char         *end = interims + ARRAY_SZ(interims);
    char                firstchar;
    char                *lastchar;
    char                *pos;

    firstchar = vterm->esbuf[0];
    lastchar = &vterm->esbuf[vterm->esbuf_len - 1];

    // too early to do anything
    if(!firstchar) return;

    // interpret ESC-M as reverse line-feed
    if(firstchar == 'M')
    {
        vterm_scroll_up(vterm);
        vterm_escape_cancel(vterm);

        return;
    }

    if(firstchar == '7')
    {
        interpret_csi_SAVECUR(vterm, 0, 0);
        vterm_escape_cancel(vterm);

        return;
    }

    if(firstchar == '8')
    {
        interpret_csi_RESTORECUR(vterm, 0, 0);
        vterm_escape_cancel(vterm);

        return;
    }

    // The ESC c sequence is RS1 reset for most
    if(firstchar == 'c')
    {
        // push in "\ec" as a safety check
        interpret_csi_RS1_xterm(vterm, XTERM_RS1);
        vterm_escape_cancel(vterm);

        return;
    }


    /*
        start of check for interims.
        if it's not these, we don't have code to handle it.
    */
    pos = interims;

    // look for intermediates we can handle
    while(pos != end)
    {
        // match found
        if(firstchar == *pos) break;
        pos++;
    }

    // we didn't find a match.  end escape mode processing.
    if(pos == end)
    {
        vterm_escape_cancel(vterm);
        return;
    }
    /* end interims check */

    // looks like an complete xterm Operating System Command
    if(firstchar == ']' && validate_xterm_escape_suffix(lastchar))
    {
        vterm->esc_handler = vterm_interpret_xterm_osc;
    }

    // we have a complete csi escape sequence: interpret it
    if(firstchar == '[' && validate_csi_escape_suffix(lastchar))
    {
        vterm->esc_handler = vterm_interpret_esc_normal;
    }

    // SCS G0 sequence - discards for now
    if(firstchar == '(' && validate_scs_escape_suffix(lastchar))
    {
        vterm->esc_handler = vterm_interpret_esc_scs;
    }

    // SCS G1 sequence - discards for now
    if(firstchar == ')' && validate_scs_escape_suffix(lastchar))
    {
        vterm->esc_handler = vterm_interpret_esc_scs;
    }

    // DCS sequence - starts in P and ends in Esc backslash
    if( firstchar == 'P'
        && vterm->esbuf_len > 2
        && vterm->esbuf[vterm->esbuf_len - 2] == '\x1B'
        && *lastchar == '\\' )
    {
        vterm->esc_handler = vterm_interpret_esc_xterm_dsc;
    }

    // if an escape handler has been configured, run it.
    if(vterm->esc_handler != NULL)
    {
        vterm->esc_handler(vterm);
        vterm_escape_cancel(vterm);

        return;
    }

    return;
}

int
vterm_interpret_esc_xterm_dsc(vterm_t *vterm)
{
    /*
        TODO

        accept DSC (ESC-P) sequences from xterm but don't do anything
        with them - just toss them to /dev/null for now.
    */

    const char  *p;

    p = vterm->esbuf + 1;

    if( *p=='+'
        && *(p+1)=='q'
        && isxdigit( *(p+2) )
        && isxdigit( *(p+3) )
        && isxdigit( *(p+4) )
        && isxdigit( *(p+5) ) )
        {
        /* we have a valid code, and we'll promptly ignore it */
        }

    return 0;
}

int
vterm_interpret_esc_normal(vterm_t *vterm)
{
    static int  csiparam[MAX_CSI_ES_PARAMS];
    int         param_count = 0;
    const char  *p;
    char        verb;

    p = vterm->esbuf + 1;
    verb = vterm->esbuf[vterm->esbuf_len - 1];

    // parse numeric parameters
    while (isdigit(*p) || *p == ';' || *p == '?')
    {
        if(*p == '?')
        {
            p++;
            continue;
        }

        if(*p == ';')
        {
            if(param_count >= MAX_CSI_ES_PARAMS) return -1;    // too long!
            csiparam[param_count++] = 0;
        }
        else
        {
            if(param_count == 0) csiparam[param_count++] = 0;

            // increaase order of prev digit (10s column, 100s column, etc...)
            csiparam[param_count-1] *= 10;
            csiparam[param_count-1] += *p - '0';
        }

        p++;
    }

    // delegate handling depending on command character (verb)
    switch (verb)
    {
        case 'b':
        {
            interpret_csi_REP(vterm, csiparam, param_count);
            break;
        }

        case 'm':
        {
            interpret_csi_SGR(vterm, csiparam, param_count);
            break;
        }

        case 'l':
        {
            interpret_dec_RM(vterm, csiparam, param_count);
            break;
        }

        case 'h':
        {
            interpret_dec_SM(vterm, csiparam, param_count);
            break;
        }

        case 'J':
        {
            interpret_csi_ED(vterm, csiparam, param_count);
            break;
        }

        case 'H':
        {
            interpret_csi_CUx(vterm, verb, csiparam, param_count);
            break;
        }

        case 'f':
        {
            interpret_csi_CUP(vterm, csiparam, param_count);
            break;
        }

        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'e':
        case 'a':
        case 'd':
        case '`':
        {
            interpret_csi_CUx(vterm, verb, csiparam, param_count);
            break;
        }

        case 'K':
        {
            interpret_csi_EL(vterm, csiparam, param_count);
            break;
        }

        case '@':
        {
            interpret_csi_ICH(vterm, csiparam, param_count);
            break;
        }

        case 'P':
        {
            interpret_csi_DCH(vterm, csiparam, param_count);
            break;
        }

        case 'L':
        {
            interpret_csi_IL(vterm, csiparam, param_count);
            break;
        }

        case 'M':
        {
            interpret_csi_DL(vterm, csiparam, param_count);
            break;
        }

        case 'X':
        {
            interpret_csi_ECH(vterm, csiparam, param_count);
            break;
        }

        case 'r':
        {
            interpret_csi_DECSTBM(vterm, csiparam, param_count);
            break;
        }

        case 's':
        {
            interpret_csi_SAVECUR(vterm, csiparam, param_count);
            break;
        }

        case 'S':
        {
            interpret_csi_SU(vterm, csiparam, param_count);
            break;
        }

        case 'u':
        {
            interpret_csi_RESTORECUR(vterm, csiparam, param_count);
            break;
        }

        case 'Z':
        {
            interpret_csi_CBT(vterm, csiparam, param_count);
            break;
        }

#ifdef _DEBUG
        default:
        {
            fprintf(stderr, "Unrecogized CSI: <%s>\n",
                vterm->esbuf);
            break;
        }
#endif
    }

    return 0;
}

static int
vterm_interpret_esc_scs(vterm_t *vterm)
{
    const char  *p;

    p = vterm->esbuf;

    // not the most elegant way to handle these.  todo: improve later.
    if(*p == '(' && p[1] == '0')
    {
        vterm->internal_state |= STATE_ALT_CHARSET;
    }

    if(*p == '(' && p[1] == 'B')
    {
        vterm->internal_state &= ~STATE_ALT_CHARSET;
    }

    // G1 sequence - unused
    if(*p == ')') {}

    p++;
    // could do something with the codes

    // return the number of bytes handled
    return 2;
}

bool
validate_csi_escape_suffix(char *lastchar)
{
    char    c = *lastchar;

    if(c >= 'a' && c <= 'z') return TRUE;
    if(c >= 'A' && c <= 'Z') return TRUE;
    if(c == '@') return TRUE;
    if(c == '`') return TRUE;

   return FALSE;
}

bool
validate_xterm_escape_suffix(char *lastchar)
{
    char    c = *lastchar;

    if(c == '\x07') return TRUE;
    if(c == '\x9c') return TRUE;

    // seems to be a VTE thing
    if(c == '\x5c')
    {
        if( *(--lastchar) == '\x1b') return TRUE;
    }

    return FALSE;
}

bool
validate_scs_escape_suffix(char *lastchar)
{
    char c = *lastchar;

    if(c == 'A') return TRUE;
    if(c == 'B') return TRUE;
    if(c == '0') return TRUE;
    if(c == '1') return TRUE;
    if(c == '2') return TRUE;

    return FALSE;
}
