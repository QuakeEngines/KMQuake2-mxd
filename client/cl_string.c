/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// cl_string.c
// string drawing and formatting functions

#include "client.h"


/*
================
TextColor
This sets the actual text color, can be called from anywhere
================
*/
void TextColor (int colornum, int *red, int *green, int *blue)
{
	switch (colornum)
	{
		case 1:		//red
			*red =	255;
			*green=	0;
			*blue =	0;
			break;
		case 2:		//green
			*red =	0;
			*green=	255;
			*blue =	0;
			break;
		case 3:		//yellow
			*red =	255;
			*green=	255;
			*blue =	0;
			break;
		case 4:		//blue
			*red =	0;
			*green=	0;
			*blue =	255;
			break;
		case 5:		//cyan
			*red =	0;
			*green=	255;
			*blue =	255;
			break;
		case 6:		//magenta
			*red =	255;
			*green=	0;
			*blue =	255;
			break;
		case 7:		//white
			*red =	255;
			*green=	255;
			*blue =	255;
			break;
		case 8:		//black
			*red =	0;
			*green=	0;
			*blue =	0;
			break;
		case 9:		//orange
			*red =	255;
			*green=	135;
			*blue =	0;
			break;
		case 0:		//gray
			*red =	155;
			*green=	155;
			*blue =	155;
			break;
		default:	//white
			*red =	255;
			*green=	255;
			*blue =	255;
			break;
	}
}


/*
================
StringSetParams
================
*/
qboolean StringSetParams (char modifier, int *red, int *green, int *blue, qboolean *bold, qboolean *shadow, qboolean *italic, qboolean *reset)
{
	if (!alt_text_color)
		alt_text_color = Cvar_Get("alt_text_color", "2", CVAR_ARCHIVE);

	switch (modifier)
	{
		case 'R': case 'r': *reset = true;  return true;
		case 'B': case 'b':	*bold = !*bold;	return true;
		case 'S': case 's':	*shadow = !*shadow; return true;
		case 'I': case 'i': *italic = !*italic; return true;
		
		case COLOR_RED:
		case COLOR_GREEN:
		case COLOR_YELLOW:
		case COLOR_BLUE:
		case COLOR_CYAN:
		case COLOR_MAGENTA:
		case COLOR_WHITE:
		case COLOR_BLACK:
		case COLOR_ORANGE:
		case COLOR_GRAY:
			TextColor(atoi(&modifier), red, green, blue);
			return true;

		case 'A': case 'a': //alt text color
			TextColor((int)alt_text_color->value, red, green, blue);
			return true;
	}
	
	return false;
}


/*
================
StringCheckParams (mxd)
================
*/
qboolean StringCheckParams(char modifier)
{
	int i; qboolean b;
	return StringSetParams(modifier, &i, &i, &i, &b, &b, &b, &b);
}


/*
================
DrawStringGeneric
================
*/
void DrawStringGeneric (int x, int y, const char *string, int alpha, textscaletype_t scaleType, qboolean altBit)
{
	// defaults
	int red = 255;
	int green = 255;
	int blue = 255;
	qboolean italic = false;
	qboolean shadow = false;
	qboolean bold = false;

	unsigned charnum = 0;
	const unsigned len = strlen(string);
	for (unsigned i = 0; i < len; i++)
	{
		char modifier = string[i];
		if (modifier & 128)
			modifier &= ~128;

		if (modifier == '^')
		{
			i++;

			qboolean reset = false;
			modifier = string[i];
			if (modifier & 128)
				modifier &= ~128;

			if (modifier != '^')
			{
				const qboolean modified = StringSetParams(modifier, &red, &green, &blue, &bold, &shadow, &italic, &reset); //mxd

				if (reset)
				{
					red = 255;
					green = 255;
					blue = 255;
					italic = false;
					shadow = false;
					bold = false;
				}

				if (modified)
					continue;

				i--;
			}
		}

		char character = string[i];
		if (bold) //mxd
			character += 128;

		float textSize, textScale;
		switch (scaleType)
		{
		case SCALETYPE_MENU:
			textSize = SCR_ScaledVideo(MENU_FONT_SIZE);
			textScale = SCR_VideoScale();
			break;

		case SCALETYPE_HUD:
			textSize = scaledHud(HUD_FONT_SIZE);
			textScale = HudScale();
			// hack for alternate text color
			if (altBit)
				character ^= 0x80;
			if (character & 128)
				TextColor((int)alt_text_color->value, &red, &green, &blue);
			break;

		case SCALETYPE_CONSOLE:
		default:
			textSize = FONT_SIZE;
			textScale = FONT_SIZE/8.0;
			// hack for alternate text color
			if (character & 128)
				TextColor((int)alt_text_color->value, &red, &green, &blue);
			break;
		}

		if (shadow)
		{
			R_DrawChar((x + charnum * textSize + textSize / 4), y + (textSize / 8),
				character, textScale, 0, 0, 0, alpha, italic, false);
		}

		R_DrawChar((x + charnum * textSize), y,
			character, textScale, red, green, blue, alpha, italic, (i == (len - 1)));

		charnum++;
	}
}
