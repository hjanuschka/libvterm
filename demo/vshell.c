/*
Copyright (C) 2009 Bryan Christ

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
This library is based on ROTE written by Bruno Takahashi C. de Oliveira
*/

#include <ncurses.h>
#include <stdio.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
// #include <limits.h>

#include "../vterm.h"
#include "../strings.h"

int screen_w, screen_h;

typedef struct
{
    WINDOW  *term_win;
}
testwin_t;

#define VWINDOW(x)  (*(WINDOW**)x)

WINDOW  *screen_wnd;

// prototypes
void    vshell_paint_screen(vterm_t *vterm);
int     vshell_resize(testwin_t *twin, vterm_t * vterm);

int main(int argc, char **argv)
{
    vterm_t     *vterm;
    int 		i, j, ch;
    ssize_t     bytes;
    int         flags = 0;
    testwin_t   *twin;
    char        *exec_path = NULL;
    char        **exec_argv = NULL;
    int         count = 1;

	setlocale(LC_ALL,"UTF-8");

    screen_wnd = initscr();
    noecho();
    start_color();
    raw();
    nodelay(stdscr, TRUE);              /*
                                           prevents getch() from blocking;
                                           rather it will return ERR when
                                           there is no keypress available
                                        */

    keypad(stdscr, TRUE);
    getmaxyx(stdscr, screen_h, screen_w);

    twin = (testwin_t*)calloc(1, sizeof(testwin_t));

    if (argc > 1)
    {
        // interate through argv[] looking for params
        for (i = 1; i < argc; i++)
        {

            if (strncmp(argv[i], "--dump", strlen("--dump")) == 0)
            {
                flags |= VTERM_FLAG_DUMP;
                continue;
            }

            if (strncmp(argv[i], "--vt100", strlen("--vt100")) == 0)
            {
                flags |= VTERM_FLAG_VT100;
                continue;
            }

            if (strncmp(argv[i], "--exec", strlen("--exec")) == 0)
            {
                // must have at least exec path
                i++;
                if (i < argc)
                {
                    exec_path = strdup(argv[i]);

                    // first arg shouldbe same as path
                    exec_argv = (char **)calloc(argc, sizeof(char *));
                    exec_argv[0] = strdup(argv[i]);
                    i++;
                }

                count = 1;
                while(i < argc)
                {
                    exec_argv[count] = strdup(argv[i]);
                    count++;
                    i++;
                }

                // this will always be the last set of params we handle
                break;
            }
        }
    }


    /*
        initialize the color pairs the way rote_vt_draw expects it. You might
        initialize them differently, but in that case you would need
        to supply a custom conversion function for rote_vt_draw to
        call when setting attributes. The idea of this "default" mapping
        is to map (fg,bg) to the color pair bg * 8 + 7 - fg. This way,
        the pair (white,black) ends up mapped to 0, which means that
        it does not need a color pair (since it is the default). Since
        there are only 63 available color pairs (and 64 possible fg/bg
        combinations), we really have to save 1 pair by assigning no pair
        to the combination white/black.
    */
    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 8; j++)
        {
            if (i != 7 || j != 0) init_pair(j*8+7-i, i, j);
        }
    }

    // paint the screen blue
    vshell_paint_screen(NULL);

    // VWINDOW(twin) = newwin(25,80,1,4);
    VWINDOW(twin) = newwin(screen_h - 2, screen_w - 2, 1, 1);

    wattrset(VWINDOW(twin), COLOR_PAIR(7*8+7-0));        // black over white
    wrefresh(VWINDOW(twin));

    // create the terminal and have it run bash

    if(exec_path != NULL)
    {
        vterm = vterm_alloc();
        vterm_set_exec(vterm, exec_path, exec_argv);
        vterm_init(vterm, 80, 25, flags);
    }
    else
    {
        vterm = vterm_create(screen_w - 2, screen_h - 2, flags);
    }

    vterm_set_colors(vterm,COLOR_WHITE,COLOR_BLACK);
    vterm_wnd_set(vterm, VWINDOW(twin));

    /*
        keep reading keypresses from the user and passing them to
        the terminal;  also, redraw the terminal to the window at each
        iteration
    */
    ch = '\0';
    while (TRUE)
    {
        bytes = vterm_read_pipe(vterm);
        if(bytes > 0)
        {
            vterm_wnd_update(vterm);
            touchwin(VWINDOW(twin));
            wrefresh(VWINDOW(twin));
            refresh();
        }

        if(bytes == -1) break;

        ch = getch();

        // handle special events like resize first
        if(ch == KEY_RESIZE)
        {
            vshell_resize(twin, vterm);
            continue;
        }

        if (ch != ERR) vterm_write_pipe(vterm,ch);
    }

    endwin();

    // printf("Pipe buffer size was %d\n\r", PIPE_BUF);

    return 0;
}

void
vshell_paint_screen(vterm_t *vterm)
{
    extern WINDOW   *screen_wnd;
    char            title[256] = " Term In A Box ";
    char            buf[254];
    int             len;
    int             offset;

    // paint the screen blue
    // attrset(COLOR_PAIR(5));  // green on black
    attrset(COLOR_PAIR(32));    // white on blue
    box(screen_wnd, 0, 0);

    // quick computer of title location
    vterm_get_title(vterm, buf, sizeof(buf));
    if(buf[0] != '\0')
    {
        sprintf(title, " %s ", buf);
    }

    len = strlen(title);
    offset = (screen_w >> 1) - (len >> 1);
    mvprintw(0, offset, title);

    sprintf(title, " %d x %d ", screen_w - 2, screen_h - 2);
    len = strlen(title);
    mvprintw(screen_h - 1, offset, title);

    refresh();

    return;
}

int
vshell_resize(testwin_t *twin, vterm_t * vterm)
{
    // pid_t   child = 0;

    getmaxyx(stdscr, screen_h, screen_w);

    vshell_paint_screen(vterm);

    wresize(VWINDOW(twin), screen_h - 2, screen_w - 2);

    vterm_resize(vterm, screen_w - 2, screen_h - 2);

    touchwin(VWINDOW(twin));
    wrefresh(VWINDOW(twin));
    refresh();

    return 0;
}

