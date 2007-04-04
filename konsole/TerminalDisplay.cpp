/*
    This file is part of Konsole, a terminal emulator for KDE.
    
    Copyright (C) 2006 by Robert Knight <robertknight@gmail.com>
    Copyright (C) 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

/*! \class TerminalDisplay

    \brief Visible screen contents

   This class is responsible to map the `_image' of a terminal emulation to the
   display. All the dependency of the emulation to a specific GUI or toolkit is
   localized here. Further, this widget has no knowledge about being part of an
   emulation, it simply work within the terminal emulation framework by exposing
   size and key events and by being ordered to show a new _image.

   <ul>
   <li> The internal _image has the size of the widget (evtl. rounded up)
   <li> The external _image used in setImage can have any size.
   <li> (internally) the external _image is simply copied to the internal
        when a setImage happens. During a resizeEvent no painting is done
        a paintEvent is expected to follow anyway.
   </ul>

   \sa Screen \sa Emulation
*/

// System
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// Qt
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegExp>
#include <QResizeEvent>
#include <QSpacerItem>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

// KDE
#include <KRun>
#include <KCursor>
#include <kdebug.h>
#include <KLocale>
#include <KNotification>
#include <KGlobalSettings>
#include <KShortcut>
#include <KIO/NetAccess>

// Konsole
#include "config.h"
#include "Filter.h"
#include "TerminalDisplay.h"
#include "konsole_wcwidth.h"
#include "ScreenWindow.h"

using namespace Konsole;

#ifndef loc
#define loc(X,Y) ((Y)*_columns+(X))
#endif

#define SCRWIDTH 16 // width of the _scrollBar

#define yMouseScroll 1

#define REPCHAR   "ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
                  "abcdefgjijklmnopqrstuvwxyz" \
                  "0123456789./+@"

extern bool true_transparency; // declared in main.characterpp and konsole_part.characterpp

// scroll increment used when dragging selection at top/bottom of window.

// static
bool TerminalDisplay::s_antialias = true;
bool TerminalDisplay::s_standalone = false;

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Colors                                     */
/*                                                                           */
/* ------------------------------------------------------------------------- */

//FIXME: the default color table is in session.C now.
//       We need a way to get rid of this one, here.


/* Note that we use ANSI color order (bgr), while IBMPC color order is (rgb)

   Code        0       1       2       3       4       5       6       7
   ----------- ------- ------- ------- ------- ------- ------- ------- -------
   ANSI  (bgr) Black   Red     Green   Yellow  Blue    Magenta Cyan    White
   IBMPC (rgb) Black   Blue    Green   Cyan    Red     Magenta Yellow  White
*/

ScreenWindow* TerminalDisplay::screenWindow() const
{
    return _screenWindow;
}
void TerminalDisplay::setScreenWindow(ScreenWindow* window)
{
    // disconnect existing screen window if any
    if ( _screenWindow )
    {
        disconnect( _screenWindow , 0 , this , 0 );
    }

    _screenWindow = window;

#warning "The order here is not specified - does it matter whether updateImage or updateLineProperties comes first?"
    connect( _screenWindow , SIGNAL(outputChanged()) , this , SLOT(updateLineProperties()) );
    connect( _screenWindow , SIGNAL(outputChanged()) , this , SLOT(updateImage()) );
}

void TerminalDisplay::setDefaultBackColor(const QColor& color)
{
  _defaultBgColor = color;
  
  QPalette p = palette();
  p.setColor( backgroundRole(), defaultBackColor() );
  setPalette( p );
}

QColor TerminalDisplay::defaultBackColor()
{
  if (_defaultBgColor.isValid())
    return _defaultBgColor;
  return _colorTable[DEFAULT_BACK_COLOR].color;
}

const ColorEntry* TerminalDisplay::colorTable() const
{
  return _colorTable;
}

void TerminalDisplay::setColorTable(const ColorEntry table[])
{
  for (int i = 0; i < TABLE_COLORS; i++) _colorTable[i] = table[i];
 
  const QPixmap* pm = 0; 
  if (!pm)
  {
    if (!true_transparency || (qAlpha(_blendColor) == 0xff))
    {
        QPalette p = palette();
        p.setColor( backgroundRole(), defaultBackColor() );
        setPalette( p );
    } else {

        //### probably buggy
        QPalette p = palette();
        p.setColor( backgroundRole(), _blendColor );
        setPalette( p );
    }
  }
  update();
}

//FIXME: add backgroundPixmapChanged.

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Font                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*
   The VT100 has 32 special graphical characters. The usual vt100 extended
   xterm fonts have these at 0x00..0x1f.

   QT's iso mapping leaves 0x00..0x7f without any changes. But the graphicals
   come in here as proper unicode characters.

   We treat non-iso10646 fonts as VT100 extended and do the requiered mapping
   from unicode to 0x00..0x1f. The remaining translation is then left to the
   QCodec.
*/

static inline bool isLineChar(Q_UINT16 c) { return ((c & 0xFF80) == 0x2500);}
static inline bool isLineCharString(const QString& string)
{
		return (string.length() > 0) && (isLineChar(string.at(0).unicode()));
}
						

// assert for i in [0..31] : vt100extended(vt100_graphics[i]) == i.

unsigned short Konsole::vt100_graphics[32] =
{ // 0/8     1/9    2/10    3/11    4/12    5/13    6/14    7/15
  0x0020, 0x25C6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
  0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
  0xF800, 0xF801, 0x2500, 0xF803, 0xF804, 0x251c, 0x2524, 0x2534,
  0x252c, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00b7
};

/*
static QChar vt100extended(QChar c)
{
  switch (c.unicode())
  {
    case 0x25c6 : return  1;
    case 0x2592 : return  2;
    case 0x2409 : return  3;
    case 0x240c : return  4;
    case 0x240d : return  5;
    case 0x240a : return  6;
    case 0x00b0 : return  7;
    case 0x00b1 : return  8;
    case 0x2424 : return  9;
    case 0x240b : return 10;
    case 0x2518 : return 11;
    case 0x2510 : return 12;
    case 0x250c : return 13;
    case 0x2514 : return 14;
    case 0x253c : return 15;
    case 0xf800 : return 16;
    case 0xf801 : return 17;
    case 0x2500 : return 18;
    case 0xf803 : return 19;
    case 0xf804 : return 20;
    case 0x251c : return 21;
    case 0x2524 : return 22;
    case 0x2534 : return 23;
    case 0x252c : return 24;
    case 0x2502 : return 25;
    case 0x2264 : return 26;
    case 0x2265 : return 27;
    case 0x03c0 : return 28;
    case 0x2260 : return 29;
    case 0x00a3 : return 30;
    case 0x00b7 : return 31;
  }
  return c;
}

static QChar identicalMap(QChar c)
{
  return c;
}
*/

void TerminalDisplay::fontChange(const QFont &)
{
  QFontMetrics fm(font());
  _fontHeight = fm.height() + _lineSpacing;

  // waba TerminalDisplay 1.123:
  // "Base character width on widest ASCII character. This prevents too wide
  //  characters in the presence of double wide (e.g. Japanese) characters."
  // Get the width from representative normal width characters
  _fontWidth = qRound((double)fm.width(REPCHAR)/(double)strlen(REPCHAR));

  _fixedFont = true;
  int fw = fm.width(REPCHAR[0]);
  for(unsigned int i=1; i< strlen(REPCHAR); i++){
    if (fw != fm.width(REPCHAR[i])){
      _fixedFont = false;
      break;
  }
  }

  if (_fontWidth>200) // don't trust unrealistic value, fallback to QFontMetrics::maxWidth()
    _fontWidth=fm.maxWidth();
  if (_fontWidth<1)
    _fontWidth=1;

  _fontAscent = fm.ascent();

  emit changedFontMetricSignal( _fontHeight, _fontWidth );
  propagateSize();
  update();
}

void TerminalDisplay::setVTFont(const QFont& f)
{
  QFont font = f;

  QFontMetrics metrics(font);

  if ( metrics.height() < height() && metrics.maxWidth() < width() )
  {
    if (!s_antialias)
        font.setStyleStrategy( QFont::NoAntialias );
  
    QFrame::setFont(font);
    fontChange(font);
  }
}

void TerminalDisplay::setFont(const QFont &)
{
  // ignore font change request if not coming from konsole itself
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                         Constructor / Destructor                          */
/*                                                                           */
/* ------------------------------------------------------------------------- */

TerminalDisplay::TerminalDisplay(QWidget *parent)
:QFrame(parent)
,_screenWindow(0)
,_allowBell(true)
,_gridLayout(0)
,_fontHeight(1)
,_fontWidth(1)
,_fontAscent(1)
,_lines(1)
,_columns(1)
,_usedLines(1)
,_usedColumns(1)
,_contentHeight(1)
,_contentWidth(1)
,_image(0)
,_resizing(false)
,_terminalSizeHint(false)
,_terminalSizeStartup(true)
,_bidiEnabled(false)
,_actSel(0)
,_wordSelectionMode(false)
,_lineSelectionMode(false)
,_preserveLineBreaks(true)
,_columnSelectionMode(false)
,_scrollbarLocation(SCROLLBAR_NONE)
,_wordCharacters(":@-./_~")
,_bellMode(BELLSYSTEM)
,_blinking(false)
,_cursorBlinking(false)
,_hasBlinkingCursor(false)
,_ctrlDrag(false)
,_cutToBeginningOfLine(false)
,_isPrinting(false)
,_printerFriendly(false)
,_printerBold(false)
,_isFixedSize(false)
,_drop(0)
,_possibleTripleClick(false)
,_resizeWidget(0)
,_resizeLabel(0)
,_resizeTimer(0)
,_outputSuspendedLabel(0)
,_lineSpacing(0)
,_colorsInverted(false)
,_rimX(1)
,_rimY(1)
,_imPreeditText(QString())
,_imPreeditLength(0)
,_imStart(0)
,_imStartLine(0)
,_imEnd(0)
,_imSelStart(0)
,_imSelEnd(0)
,_cursorLine(0)
,_cursorCol(0)
,_isIMEdit(false)
,_blendColor(qRgba(0,0,0,0xff))
,_filterChain(new TerminalImageFilterChain())
{
  // The offsets are not yet calculated.
  // Do not calculate these too often to be more smoothly when _resizing
  // konsole in opaque mode.
  _bY = _bX = 1;

  // create _scrollBar for scrolling output up and down
  // set the _scrollBar's slider to occupy the whole area of the _scrollBar initially
  _scrollBar = new QScrollBar(this);
  setScroll(0,0); 
  _scrollBar->setCursor( Qt::ArrowCursor );
  connect(_scrollBar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));

  _blinkTimer   = new QTimer(this);
  connect(_blinkTimer, SIGNAL(timeout()), this, SLOT(blinkEvent()));
  _blinkCursorTimer   = new QTimer(this);
  connect(_blinkCursorTimer, SIGNAL(timeout()), this, SLOT(blinkCursorEvent()));

  setUsesMouse(true);
  setColorTable(base_color_table); // init color table

  qApp->installEventFilter( this ); //FIXME: see below
  KCursor::setAutoHideCursor( this, true );

  setMouseTracking(true);

  // Init DnD 
  setAcceptDrops(true); // attempt
  dragInfo.state = diNone;

  setFocusPolicy( Qt::WheelFocus );
  // im
  setAttribute(Qt::WA_InputMethodEnabled, true);

  //tell Qt to automatically fill the widget with the current background colour when
  //repainting.
  //the widget may then need to repaint over some of the area in a different colour
  //but because of the double buffering there won't be any flicker
  setAutoFillBackground(true);

  _gridLayout = new QGridLayout(this);
  _gridLayout->setMargin(0);

  setLayout( _gridLayout ); 
  setLineWidth(0);

  //set up a warning message when the user presses Ctrl+S to avoid confusion
  connect( this,SIGNAL(flowControlKeyPressed(bool)),this,SLOT(outputSuspended(bool)) );
}

TerminalDisplay::~TerminalDisplay()
{
  qApp->removeEventFilter( this );
  if (_image) free(_image);

  delete _gridLayout;
  delete _outputSuspendedLabel;
  delete _filterChain;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                             Display Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/**
 A table for emulating the simple (single width) unicode drawing chars.
 It represents the 250x - 257x glyphs. If it's zero, we can't use it.
 if it's not, it's encoded as follows: imagine a 5x5 grid where the points are numbered
 0 to 24 left to top, top to bottom. Each point is represented by the corresponding bit.

 Then, the pixels basically have the following interpretation:
 _|||_
 -...-
 -...-
 -...-
 _|||_

where _ = none
      | = vertical line.
      - = horizontal line.
 */


enum LineEncode
{
    TopL  = (1<<1),
    TopC  = (1<<2),
    TopR  = (1<<3),

    LeftT = (1<<5),
    Int11 = (1<<6),
    Int12 = (1<<7),
    Int13 = (1<<8),
    RightT = (1<<9),

    LeftC = (1<<10),
    Int21 = (1<<11),
    Int22 = (1<<12),
    Int23 = (1<<13),
    RightC = (1<<14),

    LeftB = (1<<15),
    Int31 = (1<<16),
    Int32 = (1<<17),
    Int33 = (1<<18),
    RightB = (1<<19),

    BotL  = (1<<21),
    BotC  = (1<<22),
    BotR  = (1<<23)
};

#include "linefont.h"

static void drawLineChar(QPainter& paint, int x, int y, int w, int h, uchar code)
{
    //Calculate cell midpoints, end points.
    int cx = x + w/2;
    int cy = y + h/2;
    int ex = x + w - 1;
    int ey = y + h - 1;

    Q_UINT32 toDraw = LineChars[code];

    //Top _lines:
    if (toDraw & TopL)
        paint.drawLine(cx-1, y, cx-1, cy-2);
    if (toDraw & TopC)
        paint.drawLine(cx, y, cx, cy-2);
    if (toDraw & TopR)
        paint.drawLine(cx+1, y, cx+1, cy-2);

    //Bot _lines:
    if (toDraw & BotL)
        paint.drawLine(cx-1, cy+2, cx-1, ey);
    if (toDraw & BotC)
        paint.drawLine(cx, cy+2, cx, ey);
    if (toDraw & BotR)
        paint.drawLine(cx+1, cy+2, cx+1, ey);

    //Left _lines:
    if (toDraw & LeftT)
        paint.drawLine(x, cy-1, cx-2, cy-1);
    if (toDraw & LeftC)
        paint.drawLine(x, cy, cx-2, cy);
    if (toDraw & LeftB)
        paint.drawLine(x, cy+1, cx-2, cy+1);

    //Right _lines:
    if (toDraw & RightT)
        paint.drawLine(cx+2, cy-1, ex, cy-1);
    if (toDraw & RightC)
        paint.drawLine(cx+2, cy, ex, cy);
    if (toDraw & RightB)
        paint.drawLine(cx+2, cy+1, ex, cy+1);

    //Intersection points.
    if (toDraw & Int11)
        paint.drawPoint(cx-1, cy-1);
    if (toDraw & Int12)
        paint.drawPoint(cx, cy-1);
    if (toDraw & Int13)
        paint.drawPoint(cx+1, cy-1);

    if (toDraw & Int21)
        paint.drawPoint(cx-1, cy);
    if (toDraw & Int22)
        paint.drawPoint(cx, cy);
    if (toDraw & Int23)
        paint.drawPoint(cx+1, cy);

    if (toDraw & Int31)
        paint.drawPoint(cx-1, cy+1);
    if (toDraw & Int32)
        paint.drawPoint(cx, cy+1);
    if (toDraw & Int33)
        paint.drawPoint(cx+1, cy+1);

}

void TerminalDisplay::drawLineCharString(	QPainter& painter, int x, int y, const QString& str, 
									const Character* attributes)
{
		const QPen& currentPen = painter.pen();
		
		if ( attributes->rendition & RE_BOLD )
		{
			QPen boldPen(currentPen);
			boldPen.setWidth(3);
			painter.setPen( boldPen );
		}	
		
		for (int i=0 ; i < str.length(); i++)
		{
			uchar code = str[i].cell();
        	if (LineChars[code])
            	drawLineChar(painter, x + (_fontWidth*i), y, _fontWidth, _fontHeight, code);
		}

		painter.setPen( currentPen );
}

//TODO
//The old version painted the text on a character-by-character basis, this is slow and should be 
//avoided if at all possible.
//
//Investigate:
//	- Why did the old version allow double the width for characters at column 0?  I cannot
//	see any obvious visual differences
// 
// -- Robert Knight <robertknight@gmail.com>

void TerminalDisplay::drawTextFixed(QPainter& painter, int x, int y, QString& str, const Character* /*attributes*/)
{
	if ( str.length() == 0 )
			return;
		
    painter.drawText( QRect( x, y, _fontWidth*str.length(), _fontHeight ),  Qt::TextDontClip , str );
}

//OLD VERSION
//

/*void TerminalDisplay::drawTextFixed(QPainter &paint, int x, int y,
                             QString& str, const Character *attr)
{
  QString drawstr;
  unsigned int nc=0;
  int w;
  for(int i=0;i<str.length();i++)
  {
    drawstr = str.at(i);
    // Add double of the width if next c is 0;
    if ((attr+nc+1)->c) // This may access _image[_imageSize] See makeImage()
    {
      w = _fontWidth;
      nc++;
    }
    else
    {
      w = _fontWidth*2;
      nc+=2;
    }

    //Check for line-drawing char
    if (isLineChar(drawstr[0].unicode()))
    {
        uchar code = drawstr[0].characterell();
        if (LineChars[code])
        {
            drawLineChar(paint, x, y, w, _fontHeight, code);
            x += w;
            continue;
        }
    }

    paint.drawText( QRect( x, y, w, _fontHeight ), Qt::AlignHCenter | Qt::TextDontClip, drawstr );
    x += w;
  }
}*/


/*!
    attributed string draw primitive
*/

void TerminalDisplay::drawAttrStr(QPainter &paint, const QRect& rect,
                           QString& str, const Character *attr, bool pm, bool clear)
{

  //draw text fragment.
  //the basic process is:
  //	1.  save current state of painter
  //	2.  set painter properties and draw text
  //	3.  restore state of painter
  paint.save();

  int a = _fontAscent + _lineSpacing / 2;
  QColor fColor = _printerFriendly ? Qt::black : attr->foregroundColor.color(_colorTable);
  QColor bColor = attr->backgroundColor.color(_colorTable);
  QString drawstr;

  if ((attr->rendition & RE_CURSOR) && !_isPrinting)
    _cursorRect = rect;

  // Paint background
  if (!_printerFriendly)
  {
    if (attr->isTransparent(_colorTable))
    {
      if (pm)
        paint.setBackgroundMode( Qt::TransparentMode );
    }
    else
    {
      if (pm || clear || (_blinking && (attr->rendition & RE_BLINK)) ||
          attr->backgroundColor == CharacterColor(COLOR_SPACE_DEFAULT, _colorsInverted ? DEFAULT_FORE_COLOR : DEFAULT_BACK_COLOR) )

        // draw background colors with 75% opacity
        if ( true_transparency && qAlpha(_blendColor) < 0xff ) {
          QRgb col = bColor.rgb();

          Q_UINT8 salpha = 192;
          Q_UINT8 dalpha = 255 - salpha;

          int a, r, g, b;
          a = qMin( (qAlpha (col) * salpha) / 255 + (qAlpha (_blendColor) * dalpha) / 255, 255 );
          r = qMin( (qRed   (col) * salpha) / 255 + (qRed   (_blendColor) * dalpha) / 255, 255 );
          g = qMin( (qGreen (col) * salpha) / 255 + (qGreen (_blendColor) * dalpha) / 255, 255 );
          b = qMin( (qBlue  (col) * salpha) / 255 + (qBlue  (_blendColor) * dalpha) / 255, 255 );

          col = a << 24 | r << 16 | g << 8 | b;
          //int pixel = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);

          paint.fillRect(rect, QColor(col));
        } else
          paint.fillRect(rect, bColor);
    }

    QString tmpStr = str.simplified();
    if ( _isIMEdit && !tmpStr.isEmpty() ) { // imput method edit area background color
      QRect tmpRect = rect;
      if ( str != _imPreeditText ) {  // ugly hack
        tmpRect.setLeft( tmpRect.left() + _fontWidth );
        tmpRect.setWidth( tmpRect.width() + _fontWidth );
      }

      paint.fillRect( tmpRect, Qt::darkCyan );  // currently use hard code color
    }

    if ( _isIMSel && !tmpStr.isEmpty() ) { // imput method selection background color
      int x = rect.left() + ( _fontWidth * (_imSelStart - _imStart) );
      int y = rect.top();
      int w = _fontWidth * (_imSelEnd - _imSelStart);
      int h = _fontHeight;

      QRect tmpRect = QRect( x, y, w, h );
      if ( str != _imPreeditText ) {  // ugly hack
        tmpRect.setLeft( tmpRect.left() + _fontWidth );
        tmpRect.setWidth( tmpRect.width() + _fontWidth );
      }

      paint.fillRect( tmpRect, Qt::darkGray );   // currently use hard code color
    }
  }

  // Paint cursor
  if ((attr->rendition & RE_CURSOR) && !_isPrinting) {
    paint.setBackgroundMode( Qt::TransparentMode );
    int h = _fontHeight - _lineSpacing;
    QRect r(rect.x(),rect.y()+_lineSpacing/2,rect.width(),h);
    if (hasFocus())
    {
       if (!_cursorBlinking)
       {
          paint.fillRect(r, fColor);
          fColor = bColor;
       }
    }
    else
    {
       paint.setPen(fColor);
       paint.drawRect(rect.x(),rect.y()+_lineSpacing/2,rect.width()-1,h-1);
    }
  }

  // Paint text
  
  //Check & apply BOLD font
  if (attr->rendition & RE_BOLD)
  {
  		QFont currentFont = paint.font();
		currentFont.setBold(true);
  		paint.setFont( currentFont );
  }

  
  
  if (!(_blinking && (attr->rendition & RE_BLINK)))
  {
    // ### Disabled for now, since it causes problems with characters
    // that use the full width and/or height of the character cells.
    //bool shadow = ( !_isPrinting && qAlpha(_blendColor) < 0xff
    //		    && qGray( fColor.rgb() ) > 64 );
    bool shadow = false;
    paint.setPen(fColor);
    int x = rect.x();
    if (attr->isBold(_colorTable) && _printerBold)
    {
      // When printing we use a bold font for bold
      QFont f = font();
      f.setBold(true);
      paint.setFont(f);
    }
   
    if(!_fixedFont)
	{  	
	  int y = rect.y(); // top of rect
		
	  //check whether the string consists of normal text or line drawing
	  //characters.
	  if (isLineCharString( str ))
	  {
	  	drawLineCharString(paint,x,y,str,attr);
	  }
	  else
	  {
      	if ( shadow ) 
		{
        	paint.setPen( Qt::black );
        	drawTextFixed(paint, x+1, y+1, str, attr);
        	paint.setPen(fColor);
      	}
      
      	drawTextFixed(paint, x, y, str, attr);
	  }
    }
    else
    {
      // The meaning of y differs between different versions of QPainter::drawText!!
      int y = rect.y()+a; // baseline

#ifdef __GNUC__
   #warning "BiDi stuff killed, it should force TRL when !_bidiEnabled"
#endif

      if ( shadow ) {
        paint.setPen( Qt::black );

        paint.drawText(x+1,y+1, str);
        paint.setPen(fColor);
      }

      paint.drawText(x,y, str);
    }

    if (attr->isBold(_colorTable) && _isPrinting)
    {
      // When printing we use a bold font for bold
      paint.restore();
    }

    if (attr->isBold(_colorTable) && !_printerBold)
    {
      paint.setClipRect(rect);
      // On screen we use overstrike for bold
      paint.setBackgroundMode( Qt::TransparentMode );
      int x = rect.x()+1;
      if(!_fixedFont)
      {
        // The meaning of y differs between different versions of QPainter::drawText!!
        int y = rect.y(); // top of rect
        drawTextFixed(paint, x, y, str, attr);
      }
      else
      {
        // The meaning of y differs between different versions of QPainter::drawText!!
        int y = rect.y()+a; // baseline
        //### if (_bidiEnabled)
          paint.drawText( QPoint( x, y ), str );
        //else
        //###   paint.drawText(x,y, str, -1, QPainter::LTR);
      }
      paint.setClipping(false);
    }
    if (attr->rendition & RE_UNDERLINE)
      paint.drawLine(rect.left(), rect.y()+a+1,
                     rect.right(),rect.y()+a+1 );
  }

  //restore painter to state prior to drawing text
  paint.restore();
}

/*!
    Set XIM Position
*/
void TerminalDisplay::setCursorPos(const int curx, const int cury)
{
    QPoint tL  = contentsRect().topLeft();
    int    tLx = tL.x();
    int    tLy = tL.y();

    int xpos, ypos;
    ypos = _bY + tLy + _fontHeight*(cury-1) + _fontAscent;
    xpos = _bX + tLx + _fontWidth*curx;
    //setMicroFocusHint(xpos, ypos, 0, _fontHeight); //### ???
    // fprintf(stderr, "x/y = %d/%d\txpos/ypos = %d/%d\n", curx, cury, xpos, ypos);
    _cursorLine = cury;
    _cursorCol = curx;
}

//scrolls the _image by '_lines', down if _lines > 0 or up otherwise.
//
//the terminal emulation keeps track of the scrolling of the character _image as it receives input,
//and when the view is updated, it calls scrollImage() with the final scroll amount.  this improves
//performance because scrolling the display is much cheaper than re-rendering all the text for the part
//of the _image which has moved up or down.  instead only new _lines have to be drawn
//
//note:  it is important that the area of the display which is scrolled aligns properly with
//the character grid - which has a top left point at (_bX,_bY) , a cell width of _fontWidth and a cell height
//of _fontHeight).    
void TerminalDisplay::scrollImage(int _lines)
{
    if ( _lines == 0 || _image == 0 || abs(_lines) >= this->_usedLines ) return;

    QRect scrollRect;

    //scroll internal _image
    if ( _lines > 0 )
    {
        assert( (_lines*this->_usedColumns) < _imageSize ); 

        //scroll internal _image down
        memmove( _image , &_image[_lines*this->_usedColumns] , ( this->_usedLines - _lines ) * this->_usedColumns * sizeof(Character) );
 
        //set region of display to scroll, making sure that
        //the region aligns correctly to the character grid 
        scrollRect = QRect( _bX ,_bY, this->_usedColumns * _fontWidth , (this->_usedLines - _lines) * _fontHeight );

        //qDebug() << "scrolled down " << _lines << " _lines";
    }
    else
    {
        //scroll internal _image up
        memmove( &_image[ abs(_lines)*this->_usedColumns] , _image , 
                        (this->_usedLines - abs(_lines) ) * this->_usedColumns * sizeof(Character) );

        //set region of the display to scroll, making sure that
        //the region aligns correctly to the character grid
        
        QPoint topPoint( _bX , _bY + abs(_lines)*_fontHeight );

        scrollRect = QRect( topPoint , QSize( this->_usedColumns*_fontWidth , (this->_usedLines - abs(_lines)) * _fontHeight ));
        
        //qDebug() << "scrolled up " << _lines << " _lines";
    }

    //scroll the display vertically to match internal _image
    scroll( 0 , _fontHeight * (-_lines) , scrollRect );
}

void TerminalDisplay::processFilters() 
{
    _filterChain->reset();
    _filterChain->addImage(_image,_lines,_columns);
    _filterChain->process();
}

void TerminalDisplay::updateImage() 
{
  // optimization - scroll the existing _image where possible and 
  // avoid expensive text drawing for parts of the _image that 
  // can simply be moved up or down
  scrollImage( _screenWindow->scrollCount() );
  _screenWindow->resetScrollCount();

  const Character* const newimg = _screenWindow->getImage();
  int _lines = _screenWindow->windowLines();
  int _columns = _screenWindow->windowColumns();

  setScroll( _screenWindow->currentLine() , _screenWindow->lineCount() );

  if (!_image)
     updateImageSize(); // Create _image

  assert( this->_usedLines <= this->_lines );
  assert( this->_usedColumns <= this->_columns );

  int y,x,len;

  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();
  _hasBlinker = false;

  CharacterColor cf;       // undefined
  CharacterColor _clipboard;       // undefined
  int cr  = -1;   // undefined

  const int linesToUpdate = qMin(this->_lines, qMax(0,_lines  ));
  const int columnsToUpdate = qMin(this->_columns,qMax(0,_columns));

  QChar *disstrU = new QChar[columnsToUpdate];
  char *dirtyMask = (char *) malloc(columnsToUpdate+2);
  QRegion dirtyRegion;

  // debugging variable, this records the number of _lines that are found to
  // be 'dirty' ( ie. have changed from the old _image to the new _image ) and
  // which therefore need to be repainted
  int dirtyLineCount = 0;

  for (y = 0; y < linesToUpdate; y++)
  {
    const Character*       currentLine = &_image[y*this->_columns];
    const Character* const newLine = &newimg[y*_columns];

    bool updateLine = false;
    
    // The dirty mask indicates which characters need repainting. We also
    // mark surrounding neighbours dirty, in case the character exceeds
    // its cell boundaries
    memset(dirtyMask, 0, columnsToUpdate+2);
    // Two extra so that we don't have to have to care about start and end conditions
    for (x = 0; x < columnsToUpdate; x++)
    {
	if ( ( (_imPreeditLength > 0) && 
           ( ( _imStartLine == y ) && 
             ( ( _imStart < _imEnd ) && 
               ( ( x > _imStart ) ) && 
               ( x < _imEnd ) )
              || ( ( _imSelStart < _imSelEnd ) && ( ( x > _imSelStart ) ) ) ) )
            || newLine[x] != currentLine[x])
      {
         dirtyMask[x] = dirtyMask[x+1] = dirtyMask[x+2] = 1;
      }
    }
    dirtyMask++; // Position correctly

    if (!_resizing) // not while _resizing, we're expecting a paintEvent
    for (x = 0; x < columnsToUpdate; x++)
    {
      _hasBlinker |= (newLine[x].rendition & RE_BLINK);
    
      // Start drawing if this character or the next one differs.
      // We also take the next one into account to handle the situation
      // where characters exceed their cell width.
      if (dirtyMask[x])
      {
        Q_UINT16 c = newLine[x+0].character;
        if ( !c )
            continue;
        int p = 0;
        disstrU[p++] = c; //fontMap(c);
        bool lineDraw = isLineChar(c);
        bool doubleWidth = (newLine[x+1].character == 0);
        cr = newLine[x].rendition;
        _clipboard = newLine[x].backgroundColor;
        if (newLine[x].foregroundColor != cf) cf = newLine[x].foregroundColor;
        int lln = columnsToUpdate - x;
        for (len = 1; len < lln; len++)
        {
            const Character& ch = newLine[x+len];

            if (!ch.character)
                continue; // Skip trailing part of multi-col chars.

            if (  ch.foregroundColor != cf || 
                  ch.backgroundColor != _clipboard || 
                  ch.rendition != cr ||
                  !dirtyMask[x+len] || 
                  isLineChar(c) != lineDraw || 
                  (newLine[x+len+1].character == 0) != doubleWidth )
            break;

          disstrU[p++] = c; //fontMap(c);
        }

        QString unistr(disstrU, p);

        // for XIM on the spot input style
        _isIMEdit = _isIMSel = false;

        if ( _imStartLine == y ) 
        {
          if (  ( _imStart < _imEnd ) && 
                ( x >= _imStart-1 ) && 
                ( x + int( unistr.length() ) <= _imEnd ) 
             )
                _isIMEdit = true;

          if ( ( _imSelStart < _imSelEnd ) && 
               ( x >= _imStart-1 ) && 
               ( x + int( unistr.length() ) <= _imEnd ) 
             )
                _isIMSel = true;
	    }
        else if ( _imStartLine < y ) 
        {  // for word warp
          if ( _imStart < _imEnd )
            _isIMEdit = true;

          if (  _imSelStart < _imSelEnd )
            _isIMSel = true;
	    }

        bool save__fixedFont = _fixedFont;
        if (lineDraw)
           _fixedFont = false;
        if (doubleWidth)
           _fixedFont = false;

		updateLine = true;

		_fixedFont = save__fixedFont;
        x += len - 1;
      }
      
    }

	//both the top and bottom halves of double height _lines must always be redrawn
	//although both top and bottom halves contain the same characters, only 
    //the top one is actually 
	//drawn.
    if (_lineProperties.count() > y)
        updateLine |= (_lineProperties[y] & LINE_DOUBLEHEIGHT);

    // if the characters on the line are different in the old and the new _image
    // then this line must be repainted.    
    if (updateLine)
    {
        dirtyLineCount++;

        // add the area occupied by this line to the region which needs to be
        // repainted
        QRect dirtyRect = QRect( _bX+tLx , 
                                 _bY+tLy+_fontHeight*y , 
                                 _fontWidth * columnsToUpdate , 
                                 _fontHeight ); 	
    
        dirtyRegion |= dirtyRect;
    }

    dirtyMask--; // Set back

    // replace the line of characters in the old _image with the 
    // current line of the new _image 
    memcpy((void*)currentLine,(const void*)newLine,columnsToUpdate*sizeof(Character));
  }

  // debugging - display a count of the number of _lines that will need 
  // to be repainted
  // qDebug() << "dirty line count = " << dirtyLineCount;

  // if the new _image is smaller than the previous _image, then ensure that the area
  // outside the new _image is cleared 
  if ( linesToUpdate < _usedLines )
  {
    dirtyRegion |= QRect(   _bX+tLx , 
                            _bY+tLy+_fontHeight*linesToUpdate , 
                            _fontWidth * this->_columns , 
                            _fontHeight * (_usedLines-linesToUpdate) );
  }
  _usedLines = linesToUpdate;
  
  if ( columnsToUpdate < _usedColumns )
  {
    dirtyRegion |= QRect(   _bX+tLx+columnsToUpdate*_fontWidth , 
                            _bY+tLy , 
                            _fontWidth * (_usedColumns-columnsToUpdate) , 
                            _fontHeight * this->_lines );
  }
  _usedColumns = columnsToUpdate;

  // update the parts of the display which have changed
  update(dirtyRegion);

  if ( _hasBlinker && !_blinkTimer->isActive()) _blinkTimer->start( BLINK_DELAY ); 
  if (!_hasBlinker && _blinkTimer->isActive()) { _blinkTimer->stop(); _blinking = false; }
  free(dirtyMask);
  delete [] disstrU;

  showResizeNotification();
}

void TerminalDisplay::showResizeNotification()
{
  if (_resizing && _terminalSizeHint)
  {
     if (_terminalSizeStartup) {
       _terminalSizeStartup=false;
       return;
     }
     if (!_resizeWidget)
     {
        _resizeWidget = new QFrame(this);

        QFont f = KGlobalSettings::generalFont();
        int fs = f.pointSize();
        if (fs == -1)
           fs = QFontInfo(f).pointSize();
        f.setPointSize((fs*3)/2);
        f.setBold(true);
        _resizeWidget->setFont(f);
        _resizeWidget->setFrameShape((QFrame::Shape) (QFrame::Box|QFrame::Raised));
        _resizeWidget->setMidLineWidth(2);
        QBoxLayout *l = new QVBoxLayout(_resizeWidget);
	    l->setMargin(10);
        _resizeLabel = new QLabel(i18n("Size: XXX x XXX"), _resizeWidget);
        l->addWidget(_resizeLabel, 1, Qt::AlignCenter);
        _resizeWidget->setMinimumWidth(_resizeLabel->fontMetrics().width(i18n("Size: XXX x XXX"))+20);
        _resizeWidget->setMinimumHeight(_resizeLabel->sizeHint().height()+20);
        _resizeTimer = new QTimer(this);
	_resizeTimer->setSingleShot(true);
        connect(_resizeTimer, SIGNAL(timeout()), _resizeWidget, SLOT(hide()));
     }
     QString sizeStr = i18n("Size: %1 x %2", _columns, _lines);
     _resizeLabel->setText(sizeStr);
     _resizeWidget->move((width()-_resizeWidget->width())/2,
                         (height()-_resizeWidget->height())/2+20);
     _resizeWidget->show();
     _resizeTimer->start(3000);
  }
}

void TerminalDisplay::setBlinkingCursor(bool blink)
{
  _hasBlinkingCursor=blink;
  if (blink && !_blinkCursorTimer->isActive()) _blinkCursorTimer->start(1000);
  if (!blink && _blinkCursorTimer->isActive()) {
    _blinkCursorTimer->stop();
    if (_cursorBlinking)
      blinkCursorEvent();
    else
      _cursorBlinking = false;
  }
}

// paint Event ////////////////////////////////////////////////////

/*!
    The difference of this routine vs. the `setImage' is,
    that the drawing does not include a difference analysis
    between the old and the new _image. Instead, the internal
    _image is used and the painting bound by the PaintEvent box.
*/

void TerminalDisplay::paintEvent( QPaintEvent* pe )
{
  QPainter paint;
  paint.begin( this );
  paint.setBackgroundMode( Qt::TransparentMode );

  foreach (QRect rect, (pe->region() & contentsRect()).rects())
  {
    paintContents(paint, rect);
  }
  paintFilters(paint);

  drawFrame( &paint );

  // We have to make sure every single pixel is painted by the paint event.
  // To do this, we must figure out which pixels are left in the area
  // between the terminal _image and the frame border.

  // Calculate the contents rect excluding scroll bar.
  QRect innerRect = contentsRect();
  if( _scrollbarLocation != SCROLLBAR_NONE )
    innerRect.setWidth( innerRect.width() - _scrollBar->width() );

  innerRect.setWidth( innerRect.width() + 3 );
  innerRect.setHeight( innerRect.height() );

  // Calculate the emulation rect (area needed for actual terminal contents)
  QRect emurect( contentsRect().topLeft(), QSize( _columns * _fontWidth + 2 * _rimX, _lines * _fontHeight + 2 * _rimY ));

  // Now erase the remaining pixels on all sides of the emulation

  // Top
  QRect er( innerRect );
  er.setBottom( emurect.top() );
  paint.eraseRect( er );

  // Bottom
  er.setBottom( innerRect.bottom() );
  er.setTop( emurect.bottom() );
  paint.eraseRect( er );

  // Left
  er.setTop( emurect.top() );
  er.setBottom( emurect.bottom() - 1 );
  er.setRight( emurect.left() );
  paint.eraseRect( er );

  // Right
  er.setRight( innerRect.right() );
  er.setTop( emurect.top() );
  er.setBottom( emurect.bottom() - 1 );
  er.setLeft( emurect.right() );
  paint.eraseRect( er );

  paint.end();
}

void TerminalDisplay::print(QPainter &paint, bool friendly, bool exact)
{
   bool save__fixedFont = _fixedFont;
   bool save_blinking = _blinking;
   _fixedFont = false;
   _blinking = false;
   paint.setFont(font());

   _isPrinting = true;
   _printerFriendly = friendly;
   _printerBold = !exact;

   if (exact)
   {
     QPixmap pm(contentsRect().right(), contentsRect().bottom());
     pm.fill();

     QPainter pm_paint;
     pm_paint.begin(&pm);
     paintContents(pm_paint, contentsRect());
     pm_paint.end();
     paint.drawPixmap(0, 0, pm);
   }
   else
   {
     paintContents(paint, contentsRect());
   }

   _printerFriendly = false;
   _isPrinting = false;
   _printerBold = false;

   _fixedFont = save__fixedFont;
   _blinking = save_blinking;
}

FilterChain* TerminalDisplay::filterChain() const
{
    return _filterChain;
}

void TerminalDisplay::paintFilters(QPainter& painter)
{
    // iterate over hotspots identified by the display's currently active filters 
    // and draw appropriate visuals to indicate the presence of the hotspot

    QList<Filter::HotSpot*> spots = _filterChain->hotSpots();
    QListIterator<Filter::HotSpot*> iter(spots);
    while (iter.hasNext())
    {
        Filter::HotSpot* spot = iter.next();

        for ( int line = spot->startLine() ; line <= spot->endLine() ; line++ )
        {
            int startColumn = 0;
            int endColumn = _columns; // TODO use number of _columns which are actually 
                                     // occupied on this line rather than the width of the 
                                     // display in _columns

            if ( line == spot->startLine() )
                startColumn = spot->startColumn();
            if ( line == spot->endLine() )
                endColumn = spot->endColumn();

            // subtract one pixel from
            // the right and bottom so that
            // we do not overdraw adjacent
            // hotspots
            QRect r;
            r.setCoords( startColumn*_fontWidth , line*_fontHeight,
                             endColumn*_fontWidth  - 1, (line+1)*_fontHeight - 1 ); 
                                                                           
            // Links need to be underlined
            if ( spot->type() == Filter::HotSpot::Link )
            {
                QFontMetrics metrics(font());
        
                // find the baseline (which is the invisible line that the characters in the font sit on,
                // with some having tails dangling below)
                int baseline = r.bottom() - metrics.descent();
                // find the position of the underline below that
                int underlinePos = baseline + metrics.underlinePos();

                if ( r.contains( mapFromGlobal(QCursor::pos()) ) )
                    painter.drawLine( r.left() , underlinePos , 
                                      r.right() , underlinePos );
            }
            // Marker hotspots simply have a transparent rectanglular shape
            // drawn on top of them
            else if ( spot->type() == Filter::HotSpot::Marker )
            {
            //TODO - Do not use a hardcoded colour for this
                painter.fillRect(r,QBrush(QColor(255,0,0,120)));
            }
        }
    }
}
void TerminalDisplay::paintContents(QPainter &paint, const QRect &rect)
{
  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();

  int lux = qMin(_usedColumns-1, qMax(0,(rect.left()   - tLx - _bX ) / _fontWidth));
  int luy = qMin(_usedLines-1,  qMax(0,(rect.top()    - tLy - _bY  ) / _fontHeight));
  int rlx = qMin(_usedColumns-1, qMax(0,(rect.right()  - tLx - _bX ) / _fontWidth));
  int rly = qMin(_usedLines-1,  qMax(0,(rect.bottom() - tLy - _bY  ) / _fontHeight));

  QChar *disstrU = new QChar[_usedColumns];
  for (int y = luy; y <= rly; y++)
  {
    Q_UINT16 c = _image[loc(lux,y)].character;
    int x = lux;
    if(!c && x)
      x--; // Search for start of multi-column character
    for (; x <= rlx; x++)
    {
      int len = 1;
      int p = 0;
      c = _image[loc(x,y)].character;
      if (c)
         disstrU[p++] = c; //fontMap(c);
      bool lineDraw = isLineChar(c);
      bool doubleWidth = (_image[ qMin(loc(x,y)+1,_imageSize) ].character == 0);
      CharacterColor cf = _image[loc(x,y)].foregroundColor;
      CharacterColor _clipboard = _image[loc(x,y)].backgroundColor;
      UINT8 cr = _image[loc(x,y)].rendition;
	  
      while (x+len <= rlx &&
             _image[loc(x+len,y)].foregroundColor == cf &&
             _image[loc(x+len,y)].backgroundColor == _clipboard &&
             _image[loc(x+len,y)].rendition == cr &&
             (_image[ qMin(loc(x+len,y)+1,_imageSize) ].character == 0) == doubleWidth &&
             isLineChar( c = _image[loc(x+len,y)].character) == lineDraw) // Assignment!
      {
        if (c)
          disstrU[p++] = c; //fontMap(c);
        if (doubleWidth) // assert((_image[loc(x+len,y)+1].character == 0)), see above if condition
          len++; // Skip trailing part of multi-column character
        len++;
      }
      if ((x+len < _usedColumns) && (!_image[loc(x+len,y)].character))
        len++; // Adjust for trailing part of multi-column character

   	     bool save__fixedFont = _fixedFont;
         if (lineDraw)
            _fixedFont = false;
         if (doubleWidth)
            _fixedFont = false;
         QString unistr(disstrU,p);
		 
		 if (y < _lineProperties.size())
		 {
			if (_lineProperties[y] & LINE_DOUBLEWIDTH)
				paint.scale(2,1);
			
			if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
  		 		paint.scale(1,2);
		 }

		 //calculate the area in which the text will be drawn
		 QRect textArea = QRect( _bX+tLx+_fontWidth*x , _bY+tLy+_fontHeight*y , _fontWidth*len , _fontHeight);
		
		 //move the calculated area to take account of scaling applied to the painter.
		 //the position of the area from the origin (0,0) is scaled by the opposite of whatever
		 //transformation has been applied to the painter.  this ensures that 
		 //painting does actually start from textArea.topLeft() 
         //(instead of textArea.topLeft() * painter-scale)	
		 QMatrix inverted = paint.matrix().inverted();
		 textArea.moveTopLeft( inverted.map(textArea.topLeft()) );
		 
		 //paint text fragment
         drawAttrStr(	paint,
                		textArea,
                		unistr, 
						&_image[loc(x,y)], 
						0, 
						!_isPrinting );
         
		 _fixedFont = save__fixedFont;
     
		 //reset back to single-width, single-height _lines 
		 paint.resetMatrix();

		 if (y < _lineProperties.size())
		 {
			//double-height _lines are represented by two adjacent _lines 
            //containing the same characters
			//both _lines will have the LINE_DOUBLEHEIGHT attribute.  
            //If the current line has the LINE_DOUBLEHEIGHT attribute, 
            //we can therefore skip the next line
			if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
				y++;
		 }
		 
	    x += len - 1;
    }
  }
  delete [] disstrU;
}

void TerminalDisplay::blinkEvent()
{
  _blinking = !_blinking;

  //TODO:  Optimise to only repaint the areas of the widget where there is _blinking text
  //rather than repainting the whole widget.
  update();
}

void TerminalDisplay::blinkCursorEvent()
{
  _cursorBlinking = !_cursorBlinking;
  repaint(_cursorRect);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                  Resizing                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::resizeEvent(QResizeEvent*)
{
  updateImageSize();
}

void TerminalDisplay::propagateSize()
{
  if (_isFixedSize)
  {
     setSize(_columns, _lines);
     QFrame::setFixedSize(sizeHint());
     parentWidget()->adjustSize();
     parentWidget()->setFixedSize(parentWidget()->sizeHint());
     return;
  }
  if (_image)
     updateImageSize();
}

void TerminalDisplay::updateImageSize()
{
  Character* oldimg = _image;
  int oldlin = _lines;
  int oldcol = _columns;
  makeImage();
  // we copy the old _image to reduce flicker
  int lins = qMin(oldlin,_lines);
  int cols = qMin(oldcol,_columns);

  if (oldimg)
  {
    for (int lin = 0; lin < lins; lin++)
      memcpy((void*)&_image[_columns*lin],
             (void*)&oldimg[oldcol*lin],cols*sizeof(Character));
    free(oldimg); //FIXME: try new,delete
  }

  //NOTE: control flows from the back through the chest right into the eye.
  //      `emu' will call back via `setImage'.

  _resizing = (oldlin!=_lines) || (oldcol!=_columns);

  if ( _resizing )
  {
    emit changedContentSizeSignal(_contentHeight, _contentWidth); // expose resizeEvent
  }
  
  _resizing = false;
}

//showEvent and hideEvent are reimplemented here so that it appears to other classes that the 
//display has been resized when the display is hidden or shown.
//
//this allows  
//TODO: Perhaps it would be better to have separate signals for show and hide instead of using
//the same signal as the one for a content size change 
void TerminalDisplay::showEvent(QShowEvent*)
{
    emit changedContentSizeSignal(_contentHeight,_contentWidth);
}
void TerminalDisplay::hideEvent(QHideEvent*)
{
    emit changedContentSizeSignal(_contentHeight,_contentWidth);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Scrollbar                                  */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::scrollChanged(int)
{
  _screenWindow->scrollTo( _scrollBar->value() );

  // if the thumb has been moved to the bottom of the _scrollBar then set
  // the display to automatically track new output, that is, scroll down automatically
  // to how new _lines as they are added
  const bool atEndOfOutput = (_scrollBar->value() == _scrollBar->maximum());
  _screenWindow->setTrackOutput( atEndOfOutput );

  updateImage();
}

void TerminalDisplay::setScroll(int cursor, int slines)
{
  // update _scrollBar if the range or value has changed,
  // otherwise return
  //
  // setting the range or value of a _scrollBar will always trigger
  // a repaint, so it should be avoided if it is not necessary
  if ( _scrollBar->minimum() == 0         &&
       _scrollBar->maximum() == slines    &&
       _scrollBar->value()   == cursor )
  {
        //qDebug() << "no change in _scrollBar - skipping update";
        return;
  }

  disconnect(_scrollBar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));
  _scrollBar->setRange(0,slines);
  _scrollBar->setSingleStep(1);
  _scrollBar->setPageStep(_lines);
  _scrollBar->setValue(cursor);
  connect(_scrollBar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));
}

void TerminalDisplay::setScrollBarLocation(ScrollBarLocation loc)
{
  if (_scrollbarLocation == loc) return; // quickly
  _bY = _bX = 1;
  _scrollbarLocation = loc;
  calcGeometry();
  propagateSize();
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Mouse                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*!
    Three different operations can be performed using the mouse, and the
    routines in this section serve all of them:

    1) The press/release events are exposed to the application
    2) Marking (press and move left button) and Pasting (press middle button)
    3) The right mouse button is used from the configuration menu

    NOTE: During the marking process we attempt to keep the cursor within
    the bounds of the text as being displayed by setting the mouse position
    whenever the mouse has left the text area.

    Two reasons to do so:
    1) QT does not allow the `grabMouse' to confine-to the TerminalDisplay.
       Thus a `XGrapPointer' would have to be used instead.
    2) Even if so, this would not help too much, since the text area
       of the TerminalDisplay is normally not identical with it's bounds.

    The disadvantage of the current handling is, that the mouse can visibly
    leave the bounds of the widget and is then moved back. Because of the
    current construction, and the reasons mentioned above, we cannot do better
    without changing the overall construction.
*/

/*!
*/

void TerminalDisplay::mousePressEvent(QMouseEvent* ev)
{
  if ( _possibleTripleClick && (ev->button()==Qt::LeftButton) ) {
    mouseTripleClickEvent(ev);
    return;
  }

  if ( !contentsRect().contains(ev->pos()) ) return;

  int charLine;
  int charColumn;
  characterPosition(ev->pos(),charLine,charColumn);
  QPoint pos = QPoint(charColumn,charLine);

  //kDebug() << " mouse pressed at column = " << pos.x() << " , line = " << pos.y() << endl;

  if ( ev->button() == Qt::LeftButton)
  {
    _lineSelectionMode = false;
    _wordSelectionMode = false;

    emit isBusySelecting(true); // Keep it steady...
    // Drag only when the Control key is hold
    bool selected = false;
    
    // The receiver of the testIsSelected() signal will adjust
    // 'selected' accordingly.
    //emit testIsSelected(pos.x(), pos.y(), selected);
    
    selected = _screenWindow->isSelected(pos.x(),pos.y());

    if ((!_ctrlDrag || ev->modifiers() & Qt::ControlModifier) && selected ) {
      // The user clicked inside selected text
      dragInfo.state = diPending;
      dragInfo.start = ev->pos();
    }
    else {
      // No reason to ever start a drag event
      dragInfo.state = diNone;

      _preserveLineBreaks = !( ( ev->modifiers() & Qt::ControlModifier ) && !(ev->modifiers() & Qt::AltModifier) );
      _columnSelectionMode = (ev->modifiers() & Qt::AltModifier) && (ev->modifiers() & Qt::ControlModifier);

      if (_mouseMarks || (ev->modifiers() & Qt::ShiftModifier))
      {
        _screenWindow->clearSelection();

        //emit clearSelectionSignal();
        pos.ry() += _scrollBar->value();
        _iPntSel = _pntSel = pos;
        _actSel = 1; // left mouse button pressed but nothing selected yet.
        grabMouse(   /*crossCursor*/  ); // handle with care!
      }
      else
      {
        emit mouseSignal( 0, charColumn + 1, charLine + 1 +_scrollBar->value() -_scrollBar->maximum() , 0);
      }
    }
  }
  else if ( ev->button() == Qt::MidButton )
  {
    if ( _mouseMarks || (!_mouseMarks && (ev->modifiers() & Qt::ShiftModifier)) )
      emitSelection(true,ev->modifiers() & Qt::ControlModifier);
    else
      emit mouseSignal( 1, charColumn +1, charLine +1 +_scrollBar->value() -_scrollBar->maximum() , 0);
  }
  else if ( ev->button() == Qt::RightButton )
  {
    if (_mouseMarks || (ev->modifiers() & Qt::ShiftModifier)) {
      _configureRequestPoint = QPoint( ev->x(), ev->y() );
      emit configureRequest( this, ev->modifiers()&(Qt::ShiftModifier|Qt::ControlModifier), ev->x(), ev->y() );
    }
    else
      emit mouseSignal( 2, charColumn +1, charLine +1 +_scrollBar->value() -_scrollBar->maximum() , 0);
  }
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent* ev)
{
  int charLine = 0;
  int charColumn = 0;

  characterPosition(ev->pos(),charLine,charColumn); 

  // handle filters
  // change link hot-spot appearence on mouse-over
  Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine,charColumn);
  if ( spot && spot->type() == Filter::HotSpot::Link)
  {
    _mouseOverHotspotArea.setCoords( qMin(spot->startColumn() , spot->endColumn()) * _fontWidth,
                                     spot->startLine() * _fontHeight,
                                     qMax(spot->startColumn() , spot->endColumn()) * _fontHeight,
                                     (spot->endLine()+1) * _fontHeight );

    setCursor( Qt::PointingHandCursor );

    // display tooltips when mousing over links
    // TODO: Extend this to work with filter types other than links
    const QString& tooltip = spot->tooltip();
    if ( !tooltip.isEmpty() )
    {
        QToolTip::showText( mapToGlobal(ev->pos()) , tooltip , this , _mouseOverHotspotArea );
    }

    update( _mouseOverHotspotArea );
  }
  else if ( _mouseOverHotspotArea.isValid() )
  {
        unsetCursor();

        update( _mouseOverHotspotArea );
        // set hotspot area to an invalid rectangle
        _mouseOverHotspotArea = QRect();
  }
  
  // for auto-hiding the cursor, we need mouseTracking
  if (ev->buttons() == Qt::NoButton ) return;

  // if the terminal is interested in mouse movements 
  // then emit a mouse movement signal, unless the shift
  // key is being held down, which overrides this.
  if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier))
  {
	int button = 3;
	if (ev->buttons() & Qt::LeftButton)
		button = 0;
	if (ev->buttons() & Qt::MidButton)
		button = 1;
	if (ev->buttons() & Qt::RightButton)
		button = 2;

        
        emit mouseSignal( button, 
                        charColumn + 1,
                        charLine + 1 +_scrollBar->value() -_scrollBar->maximum(),
			 1 );
      
	return;
  }

  
      
  if (dragInfo.state == diPending) {
    // we had a mouse down, but haven't confirmed a drag yet
    // if the mouse has moved sufficiently, we will confirm

   int distance = KGlobalSettings::dndEventDelay();
   if ( ev->x() > dragInfo.start.x() + distance || ev->x() < dragInfo.start.x() - distance ||
        ev->y() > dragInfo.start.y() + distance || ev->y() < dragInfo.start.y() - distance) {
      // we've left the drag square, we can start a real drag operation now
      emit isBusySelecting(false); // Ok.. we can breath again.
      _screenWindow->clearSelection();
      //emit clearSelectionSignal();
      doDrag();
    }
    return;
  } else if (dragInfo.state == diDragging) {
    // this isn't technically needed because mouseMoveEvent is suppressed during
    // Qt drag operations, replaced by dragMoveEvent
    return;
  }

  if (_actSel == 0) return;

 // don't extend selection while pasting
  if (ev->buttons() & Qt::MidButton) return;

  extendSelection( ev->pos() );
}

void TerminalDisplay::setSelectionEnd()
{
  extendSelection( _configureRequestPoint );
}

void TerminalDisplay::extendSelection( QPoint pos )
{
  //if ( !contentsRect().contains(ev->pos()) ) return;
  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();
  int    scroll = _scrollBar->value();

  // we're in the process of moving the mouse with the left button pressed
  // the mouse cursor will kept caught within the bounds of the text in
  // this widget.

  // Adjust position within text area bounds. See FIXME above.
  QPoint oldpos = pos;
  if ( pos.x() < tLx+_bX )                  pos.setX( tLx+_bX );
  if ( pos.x() > tLx+_bX+_usedColumns*_fontWidth-1 ) pos.setX( tLx+_bX+_usedColumns*_fontWidth );
  if ( pos.y() < tLy+_bY )                   pos.setY( tLy+_bY );
  if ( pos.y() > tLy+_bY+_usedLines*_fontHeight-1 )    pos.setY( tLy+_bY+_usedLines*_fontHeight-1 );

  // check if we produce a mouse move event by this
  if ( pos != oldpos ) cursor().setPos(mapToGlobal(pos));

  if ( pos.y() == tLy+_bY+_usedLines*_fontHeight-1 )
  {
    _scrollBar->setValue(_scrollBar->value()+yMouseScroll); // scrollforward
  }
  if ( pos.y() == tLy+_bY )
  {
    _scrollBar->setValue(_scrollBar->value()-yMouseScroll); // scrollback
  }

  int charColumn = 0;
  int charLine = 0;
  characterPosition(pos,charLine,charColumn);

  QPoint here = QPoint(charColumn,charLine); //QPoint((pos.x()-tLx-_bX+(_fontWidth/2))/_fontWidth,(pos.y()-tLy-_bY)/_fontHeight);
  QPoint ohere;
  QPoint _iPntSelCorr = _iPntSel;
  _iPntSelCorr.ry() -= _scrollBar->value();
  QPoint _pntSelCorr = _pntSel;
  _pntSelCorr.ry() -= _scrollBar->value();
  bool swapping = false;

  if ( _wordSelectionMode )
  {
    // Extend to word boundaries
    int i;
    int selClass;

    bool left_not_right = ( here.y() < _iPntSelCorr.y() ||
	   here.y() == _iPntSelCorr.y() && here.x() < _iPntSelCorr.x() );
    bool old_left_not_right = ( _pntSelCorr.y() < _iPntSelCorr.y() ||
	   _pntSelCorr.y() == _iPntSelCorr.y() && _pntSelCorr.x() < _iPntSelCorr.x() );
    swapping = left_not_right != old_left_not_right;

    // Find left (left_not_right ? from here : from start)
    QPoint left = left_not_right ? here : _iPntSelCorr;
    i = loc(left.x(),left.y());
    if (i>=0 && i<=_imageSize) {
      selClass = charClass(_image[i].character);
      while ( ((left.x()>0) || (left.y()>0 && (_lineProperties[left.y()-1] & LINE_WRAPPED) )) 
					  && charClass(_image[i-1].character) == selClass )
      { i--; if (left.x()>0) left.rx()--; else {left.rx()=_usedColumns-1; left.ry()--;} }
    }

    // Find left (left_not_right ? from start : from here)
    QPoint right = left_not_right ? _iPntSelCorr : here;
    i = loc(right.x(),right.y());
    if (i>=0 && i<=_imageSize) {
      selClass = charClass(_image[i].character);
      while( ((right.x()<_usedColumns-1) || (right.y()<_usedLines-1 && (_lineProperties[right.y()] & LINE_WRAPPED) )) 
					  && charClass(_image[i+1].character) == selClass )
      { i++; if (right.x()<_usedColumns-1) right.rx()++; else {right.rx()=0; right.ry()++; } }
    }

    // Pick which is start (ohere) and which is extension (here)
    if ( left_not_right )
    {
      here = left; ohere = right;
    }
    else
    {
      here = right; ohere = left;
    }
    ohere.rx()++;
  }

  if ( _lineSelectionMode )
  {
    // Extend to complete line
    bool above_not_below = ( here.y() < _iPntSelCorr.y() );

    QPoint above = above_not_below ? here : _iPntSelCorr;
    QPoint below = above_not_below ? _iPntSelCorr : here;

    while (above.y()>0 && (_lineProperties[above.y()-1] & LINE_WRAPPED) )
      above.ry()--;
    while (below.y()<_usedLines-1 && (_lineProperties[below.y()] & LINE_WRAPPED) )
      below.ry()++;

    above.setX(0);
    below.setX(_usedColumns-1);

    // Pick which is start (ohere) and which is extension (here)
    if ( above_not_below )
    {
      here = above; ohere = below;
    }
    else
    {
      here = below; ohere = above;
    }

    QPoint newSelBegin = QPoint( ohere.x(), ohere.y() );
    swapping = !(_tripleSelBegin==newSelBegin);
    _tripleSelBegin = newSelBegin;

    ohere.rx()++;
  }

  int offset = 0;
  if ( !_wordSelectionMode && !_lineSelectionMode )
  {
    int i;
    int selClass;

    bool left_not_right = ( here.y() < _iPntSelCorr.y() ||
	   here.y() == _iPntSelCorr.y() && here.x() < _iPntSelCorr.x() );
    bool old_left_not_right = ( _pntSelCorr.y() < _iPntSelCorr.y() ||
	   _pntSelCorr.y() == _iPntSelCorr.y() && _pntSelCorr.x() < _iPntSelCorr.x() );
    swapping = left_not_right != old_left_not_right;

    // Find left (left_not_right ? from here : from start)
    QPoint left = left_not_right ? here : _iPntSelCorr;

    // Find left (left_not_right ? from start : from here)
    QPoint right = left_not_right ? _iPntSelCorr : here;
    if ( right.x() > 0 && !_columnSelectionMode )
    {
      i = loc(right.x(),right.y());
      if (i>=0 && i<=_imageSize) {
        selClass = charClass(_image[i-1].character);
        if (selClass == ' ')
        {
          while ( right.x() < _usedColumns-1 && charClass(_image[i+1].character) == selClass && (right.y()<_usedLines-1) && 
						  !(_lineProperties[right.y()] & LINE_WRAPPED))
          { i++; right.rx()++; }
          if (right.x() < _usedColumns-1)
            right = left_not_right ? _iPntSelCorr : here;
          else
            right.rx()++;  // will be balanced later because of offset=-1;
        }
      }
    }

    // Pick which is start (ohere) and which is extension (here)
    if ( left_not_right )
    {
      here = left; ohere = right; offset = 0;
    }
    else
    {
      here = right; ohere = left; offset = -1;
    }
  }

  if ((here == _pntSelCorr) && (scroll == _scrollBar->value())) return; // not moved

  if (here == ohere) return; // It's not left, it's not right.

  if ( _actSel < 2 || swapping )
  {
    if ( _columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode )
    {
       _screenWindow->setSelectionStart( ohere.x() , ohere.y() , true );
      //emit beginSelectionSignal( ohere.x(), ohere.y(), true );
    }
    else
    {
       _screenWindow->setSelectionStart( ohere.x()-1-offset , ohere.y() , false );
       //emit beginSelectionSignal( ohere.x()-1-offset, ohere.y(), false );
    }

  }

  _actSel = 2; // within selection
  _pntSel = here;
  _pntSel.ry() += _scrollBar->value();

  if ( _columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode )
  {
    _screenWindow->setSelectionEnd( here.x() , here.y() );
    //emit extendSelectionSignal( here.x(), here.y() );
  }
  else
  {
    _screenWindow->setSelectionEnd( here.x()+offset , here.y() );
    //emit extendSelectionSignal( here.x()+offset, here.y() );
  }

}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* ev)
{
    int charLine;
    int charColumn;
    characterPosition(ev->pos(),charLine,charColumn);

    // handle filters
    Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine,charColumn);
    if ( spot )
    {
        if ( ev->button() == Qt::LeftButton )
        {
            spot->activate();
        }
        else if ( ev->button() == Qt::RightButton )
        {
            //TODO - Show context menu with appropriate actions for hotspot.
        }
    }

  if ( ev->button() == Qt::LeftButton)
  {
    emit isBusySelecting(false); 
    if(dragInfo.state == diPending)
    {
      // We had a drag event pending but never confirmed.  Kill selection
      _screenWindow->clearSelection();
      //emit clearSelectionSignal();
    }
    else
    {
      if ( _actSel > 1 )
      {
          setSelection( _screenWindow->selectedText(_preserveLineBreaks) );
          //emit endSelectionSignal(_preserveLineBreaks);
      }

      _actSel = 0;

      //FIXME: emits a release event even if the mouse is
      //       outside the range. The procedure used in `mouseMoveEvent'
      //       applies here, too.

      if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier))
        emit mouseSignal( 3, // release
                        charColumn + 1,
                        charLine + 1 +_scrollBar->value() -_scrollBar->maximum() , 0);

      releaseMouse();
    }
    dragInfo.state = diNone;
  }
  
  
  if ( !_mouseMarks && ((ev->button() == Qt::RightButton && !(ev->modifiers() & Qt::ShiftModifier))
                        || ev->button() == Qt::MidButton) ) 
  {
    emit mouseSignal( 3, charColumn + 1, charLine + 1 +_scrollBar->value() -_scrollBar->maximum() , 0);
    releaseMouse();
  }
}

void TerminalDisplay::characterPosition(QPoint widgetPoint,int& line,int& column)
{
    column = (widgetPoint.x()-contentsRect().left()-_bX) / _fontWidth;
    line = (widgetPoint.y()-contentsRect().top()-_bY) / _fontHeight;

    if ( line < 0 )
        line = 0;
    if ( column < 0 )
        column = 0;

    if ( line >= _usedLines )
        line = _usedLines-1;

    if ( column >= _usedColumns )
        column = _usedColumns-1;
}

void TerminalDisplay::updateLineProperties()
{
    _lineProperties = _screenWindow->getLineProperties();    
}

void TerminalDisplay::mouseDoubleClickEvent(QMouseEvent* ev)
{
  if ( ev->button() != Qt::LeftButton) return;
  
  int charLine = 0;
  int charColumn = 0;

  characterPosition(ev->pos(),charLine,charColumn);

  QPoint pos(charColumn,charLine);

  // pass on double click as two clicks.
  if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier))
  {
    // Send just _ONE_ click event, since the first click of the double click
    // was already sent by the click handler!
    emit mouseSignal( 0, pos.x()+1, pos.y()+1 +_scrollBar->value() -_scrollBar->maximum(),0 ); // left button
    return;
  }

  _screenWindow->clearSelection();
  //emit clearSelectionSignal();
  QPoint bgnSel = pos;
  QPoint endSel = pos;
  int i = loc(bgnSel.x(),bgnSel.y());
  _iPntSel = bgnSel;
  _iPntSel.ry() += _scrollBar->value();

  _wordSelectionMode = true;

  // find word boundaries...
  int selClass = charClass(_image[i].character);
  {
     // find the start of the word
     int x = bgnSel.x();
     while ( ((x>0) || (bgnSel.y()>0 && (_lineProperties[bgnSel.y()-1] & LINE_WRAPPED) )) 
					 && charClass(_image[i-1].character) == selClass )
     {  
       i--; 
       if (x>0) 
           x--; 
       else 
       {
           x=_usedColumns-1; 
           bgnSel.ry()--;
       } 
     }

     bgnSel.setX(x);
     _screenWindow->setSelectionStart( bgnSel.x() , bgnSel.y() , false );
     //emit beginSelectionSignal( bgnSel.x(), bgnSel.y(), false );

     // find the end of the word
     i = loc( endSel.x(), endSel.y() );
     x = endSel.x();
     while( ((x<_usedColumns-1) || (endSel.y()<_usedLines-1 && (_lineProperties[endSel.y()] & LINE_WRAPPED) )) 
					 && charClass(_image[i+1].character) == selClass )
     { 
         i++; 
         if (x<_usedColumns-1) 
             x++; 
         else 
         {  
             x=0; 
             endSel.ry()++; 
         } 
     }

     endSel.setX(x);

     // In word selection mode don't select @ (64) if at end of word.
     if ( ( QChar( _image[i].character ) == '@' ) && ( ( endSel.x() - bgnSel.x() ) > 0 ) )
       endSel.setX( x - 1 );


     _actSel = 2; // within selection
     
     _screenWindow->setSelectionEnd( endSel.x() , endSel.y() );
     //emit extendSelectionSignal( endSel.x(), endSel.y() );
    
     setSelection( _screenWindow->selectedText(_preserveLineBreaks) ); 
     //emit endSelectionSignal(_preserveLineBreaks);
   }

  _possibleTripleClick=true;
  QTimer::singleShot(QApplication::doubleClickInterval(),this,SLOT(tripleClickTimeout()));
}

void TerminalDisplay::wheelEvent( QWheelEvent* ev )
{
  if (ev->orientation() != Qt::Vertical)
    return;

  if ( _mouseMarks )
    _scrollBar->event(ev);
  else
  {
    int charLine;
    int charColumn;
    characterPosition( ev->pos() , charLine , charColumn );
    
    emit mouseSignal( ev->delta() > 0 ? 4 : 5, charColumn + 1, charLine + 1 +_scrollBar->value() -_scrollBar->maximum() , 0);
  }
}

void TerminalDisplay::tripleClickTimeout()
{
  _possibleTripleClick=false;
}

void TerminalDisplay::mouseTripleClickEvent(QMouseEvent* ev)
{
  int charLine;
  int charColumn;
  characterPosition(ev->pos(),charLine,charColumn);
  _iPntSel = QPoint(charColumn,charLine);

  _screenWindow->clearSelection();
  //emit clearSelectionSignal();

  _lineSelectionMode = true;
  _wordSelectionMode = false;

  _actSel = 2; // within selection
  emit isBusySelecting(true); // Keep it steady...

  while (_iPntSel.y()>0 && (_lineProperties[_iPntSel.y()-1] & LINE_WRAPPED) )
    _iPntSel.ry()--;
  if (_cutToBeginningOfLine) {
    // find word boundary start
    int i = loc(_iPntSel.x(),_iPntSel.y());
    int selClass = charClass(_image[i].character);
    int x = _iPntSel.x();
    while ( ((x>0) || (_iPntSel.y()>0 && (_lineProperties[_iPntSel.y()-1] & LINE_WRAPPED) )) 
					&& charClass(_image[i-1].character) == selClass )
    { i--; if (x>0) x--; else {x=_columns-1; _iPntSel.ry()--;} }

    _screenWindow->setSelectionStart( x , _iPntSel.y() , false );
    //emit beginSelectionSignal( x, _iPntSel.y(), false );
    _tripleSelBegin = QPoint( x, _iPntSel.y() );
  }
  else {
    _screenWindow->setSelectionStart( 0 , _iPntSel.y() , false );
    //emit beginSelectionSignal( 0, _iPntSel.y(), false );
    _tripleSelBegin = QPoint( 0, _iPntSel.y() );
  }

  while (_iPntSel.y()<_lines-1 && (_lineProperties[_iPntSel.y()] & LINE_WRAPPED) )
    _iPntSel.ry()++;
  
  _screenWindow->setSelectionEnd( _columns - 1 , _iPntSel.y() );
  //emit extendSelectionSignal( _columns-1, _iPntSel.y() );

  setSelection(_screenWindow->selectedText(_preserveLineBreaks));
  //emit endSelectionSignal(_preserveLineBreaks);

  _iPntSel.ry() += _scrollBar->value();
}


bool TerminalDisplay::focusNextPrevChild( bool next )
{
  if (next)
    return false; // This disables changing the active part in konqueror
                  // when pressing Tab
  return QFrame::focusNextPrevChild( next );
}


int TerminalDisplay::charClass(UINT16 ch) const
{
    QChar qch=QChar(ch);
    if ( qch.isSpace() ) return ' ';

    if ( qch.isLetterOrNumber() || _wordCharacters.contains(qch, Qt::CaseInsensitive ) )
    return 'a';

    // Everything else is weird
    return 1;
}

void TerminalDisplay::setWordCharacters(QString wc)
{
	_wordCharacters = wc;
}

void TerminalDisplay::setUsesMouse(bool on)
{
  _mouseMarks = on;
  setCursor( _mouseMarks ? Qt::IBeamCursor : Qt::ArrowCursor );
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                               Clipboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#undef KeyPress

#if 0
void TerminalDisplay::emitText(QString text)
{
  if (!text.isEmpty()) {
    QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, text);
    emit keyPressedSignal(&e); // expose as a big fat keypress event
  }
}
#endif


void TerminalDisplay::emitSelection(bool useXselection,bool appendReturn)
// Paste Clipboard by simulating keypress events
{
  QString text = QApplication::clipboard()->text(useXselection ? QClipboard::Selection :
                                                                 QClipboard::Clipboard);
  if(appendReturn)
    text.append("\r");
  if ( ! text.isEmpty() )
  {
    text.replace("\n", "\r");
    QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, text);
    emit keyPressedSignal(&e); // expose as a big fat keypress event
    
    _screenWindow->clearSelection();
    //emit clearSelectionSignal();
  }
}

void TerminalDisplay::setSelection(const QString& t)
{
  QApplication::clipboard()->setText(t, QClipboard::Selection);
}

void TerminalDisplay::copyClipboard()
{
  Q_ASSERT( _screenWindow );

  QString text = _screenWindow->selectedText(true);
  QApplication::clipboard()->setText(text);
}

void TerminalDisplay::pasteClipboard()
{
  emitSelection(false,false);
}

void TerminalDisplay::pasteSelection()
{
  emitSelection(true,false);
}

void TerminalDisplay::onClearSelection()
{
  _screenWindow->clearSelection();
  //emit clearSelectionSignal();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Keyboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

//FIXME: an `eventFilter' has been installed instead of a `keyPressEvent'
//       due to a bug in `QT' or the ignorance of the author to prevent
//       repaint events being emitted to the screen whenever one leaves
//       or reenters the screen to/from another application.
//
//   Troll says one needs to change focusInEvent() and focusOutEvent(),
//   which would also let you have an in-focus cursor and an out-focus
//   cursor like xterm does.

// for the auto-hide cursor feature, I added empty focusInEvent() and
// focusOutEvent() so that update() isn't called.
// For auto-hide, we need to get keypress-events, but we only get them when
// we have focus.

bool TerminalDisplay::eventFilter( QObject *obj, QEvent *e )
{
  if ( (e->type() == QEvent::Accel ||
       e->type() == QEvent::AccelAvailable ) && qApp->focusWidget() == this )
  {

      static_cast<QKeyEvent *>( e )->ignore();
      return false;
  }
  if ( obj != this /* when embedded */ && obj != parent() /* when standalone */ )
      return false; // not us
  if ( e->type() == QEvent::KeyPress )
  {
    QKeyEvent* ke = (QKeyEvent*)e;

	if (ke->modifiers() & Qt::ControlModifier)
	{
		if ( ke->key() == Qt::Key_S )
				emit flowControlKeyPressed(true /*output suspended*/);
		if ( ke->key() == Qt::Key_Q )
				emit flowControlKeyPressed(false /*output enabled*/);
	}
	
    _actSel=0; // Key stroke implies a screen update, so TerminalDisplay won't
              // know where the current selection is.

    if (_hasBlinkingCursor) {
      _blinkCursorTimer->start(BLINK_DELAY);
      if (_cursorBlinking)
        blinkCursorEvent();
      else
        _cursorBlinking = false;
    }

    emit keyPressedSignal(ke); // expose

    // in Qt2 when key events were propagated up the tree
    // (unhandled? -> parent widget) they passed the event filter only once at
    // the beginning. in qt3 this has changed, that is, the event filter is
    // called each time the event is sent (see loop in QApplication::notify,
    // when internalNotify() is called for KeyPress, whereas internalNotify
    // activates also the global event filter) . That's why we stop propagation
    // here.
    return true;
  }
  
  return QFrame::eventFilter( obj, e );
}

void TerminalDisplay::inputMethodEvent ( QInputMethodEvent *  )
{
#ifdef __GNUC__
   #warning "FIXME: Port the IM stuff!"
#endif
}

#if 0
void TerminalDisplay::imStartEvent( QIMEvent */*e*/ )
{
  _imStart = _cursorCol;
  _imStartLine = _cursorLine;
  _imPreeditLength = 0;

  _imEnd = _imSelStart = _imSelEnd = 0;
  _isIMEdit = _isIMSel = false;
}

void TerminalDisplay::imComposeEvent( QIMEvent *e )
{
  QString text.characterlear();
  if ( _imPreeditLength > 0 ) {
    text.fill( '\010', _imPreeditLength );
  }

  _imEnd = _imStart + string_width( e->text() );

  QString tmpStr = e->text().left( e->cursorPos() );
  _imSelStart = _imStart + string_width( tmpStr );

  tmpStr = e->text().mid( e->cursorPos(), e->selectionLength() );
  _imSelEnd = _imSelStart + string_width( tmpStr );
  _imPreeditLength = e->text().length();
  _imPreeditText = e->text();
  text += e->text();

  if ( text.length() > 0 ) {
    QKeyEvent ke( QEvent::KeyPress, 0, -1, 0, text );
    emit keyPressedSignal( &ke );
  }
}

void TerminalDisplay::imEndEvent( QIMEvent *e )
{
  QString text.characterlear();
  if ( _imPreeditLength > 0 ) {
      text.fill( '\010', _imPreeditLength );
  }

  _imEnd = _imSelStart = _imSelEnd = 0;
  text += e->text();
  if ( text.length() > 0 ) {
    QKeyEvent ke( QEvent::KeyPress, 0, -1, 0, text );
    emit keyPressedSignal( &ke );
  }

  QPoint tL  = contentsRect().topLeft();
  int tLx = tL.x();
  int tLy = tL.y();

  QRect repaintRect = QRect( _bX+tLx, _bY+tLy+_fontHeight*_imStartLine,
                             contentsRect().width(), contentsRect().height() );
  _imStart = 0;
  _imPreeditLength = 0;

  _isIMEdit = _isIMSel = false;
  repaint( repaintRect, true );
}
#endif

// Override any Ctrl+<key> accelerator when pressed with the keyboard
// focus in TerminalDisplay, so that the key will be passed to the terminal instead.
bool TerminalDisplay::event( QEvent *e )
{
  if ( e->type() == QEvent::AccelOverride )
  {
    QKeyEvent *ke = static_cast<QKeyEvent *>( e );
    int keyCodeQt = ke->key() | ke->modifiers();

    if ( !standalone() && (ke->modifiers() == Qt::ControlModifier) )
    {
      ke->accept();
      return true;
    }

    // Override any of the following accelerators:
    switch ( keyCodeQt )
    {
      case Qt::Key_Tab:
      case Qt::Key_Delete:
        ke->accept();
        return true;
    }
  }
  return QFrame::event( e );
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                  Frame                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::frameChanged()
{
  propagateSize();
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Sound                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::setBellMode(int mode)
{
  _bellMode=mode;
}

void TerminalDisplay::enableBell()
{
    _allowBell = true;
}



void TerminalDisplay::bell(const QString& message)
{
  if (_bellMode==BELLNONE) return;

  //limit Bell sounds / visuals etc. to max 1 per second.
  //...mainly for sound effects where rapid bells in sequence produce a horrible noise
  if ( _allowBell )
  {
    _allowBell = false;
    QTimer::singleShot(500,this,SLOT(enableBell()));
 
    kDebug(1211) << __FUNCTION__ << endl;

    if (_bellMode==BELLSYSTEM) {
                KNotification::beep();
    } else if (_bellMode==BELLNOTIFY) {
            KNotification::event("BellVisible", message,QPixmap(),this);
      } else if (_bellMode==BELLVISUAL) {
        swapColorTable();
        QTimer::singleShot(200,this,SLOT(swapColorTable()));
    }
  }
}

void TerminalDisplay::swapColorTable()
{
  ColorEntry color = _colorTable[1];
  _colorTable[1]=_colorTable[0];
  _colorTable[0]= color;
  _colorsInverted = !_colorsInverted;
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                 Auxiluary                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::clearImage()
// initialize the _image
// for internal use only
{
  // We initialize _image[_imageSize] too. See makeImage()
  for (int i = 0; i <= _imageSize; i++)
  {
    _image[i].character = ' ';
    _image[i].foregroundColor = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_FORE_COLOR);
    _image[i].backgroundColor = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_BACK_COLOR);
    _image[i].rendition = DEFAULT_RENDITION;
  }
}

// Create Image ///////////////////////////////////////////////////////

void TerminalDisplay::calcGeometry()
{
  _scrollBar->resize(QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent),
                    contentsRect().height());
  switch(_scrollbarLocation)
  {
    case SCROLLBAR_NONE :
     _bX = _rimX;
     _contentWidth = contentsRect().width() - 2 * _rimX;
     _scrollBar->hide();
     break;
    case SCROLLBAR_LEFT :
     _bX = _rimX+_scrollBar->width();
     _contentWidth = contentsRect().width() - 2 * _rimX - _scrollBar->width();
     _scrollBar->move(contentsRect().topLeft());
     _scrollBar->show();
     break;
    case SCROLLBAR_RIGHT:
     _bX = _rimX;
     _contentWidth = contentsRect().width()  - 2 * _rimX - _scrollBar->width();
     _scrollBar->move(contentsRect().topRight() - QPoint(_scrollBar->width()-1,0));
     _scrollBar->show();
     break;
  }

  //FIXME: support 'rounding' styles
  _bY = _rimY;
  _contentHeight = contentsRect().height() - 2 * _rimY + /* mysterious */ 1;

  if (!_isFixedSize)
  {
     // ensure that display is always at least one column wide
     _columns = qMax(1,_contentWidth / _fontWidth);
     _usedColumns = qMin(_usedColumns,_columns);
     
     // ensure that display is always at least one line high
     _lines = qMax(1,_contentHeight / _fontHeight);
     _usedLines = qMin(_usedLines,_lines);
  }
}

void TerminalDisplay::makeImage()
{
  calcGeometry();

  // confirm that array will be of non-zero size, since the painting code 
  // assumes a non-zero array length
  assert( _lines > 0 && _columns > 0 );
  assert( _usedLines <= _lines && _usedColumns <= _columns );

  _imageSize=_lines*_columns;
  
  // We over-commit 1 character so that we can be more relaxed in dealing with
  // certain boundary conditions: _image[_imageSize] is a valid but unused position
  _image = (Character*) malloc((_imageSize+1)*sizeof(Character));
  clearImage();
}

// calculate the needed size
void TerminalDisplay::setSize(int cols, int lins)
{
  int deltaColumns = cols - _columns;
  int deltaLines = lins - _lines;

  _size = QSize( (deltaColumns * _fontWidth) + width() ,
				  (deltaLines * _fontHeight) + height() );

  updateGeometry();
}

void TerminalDisplay::setFixedSize(int cols, int lins)
{
  _isFixedSize = true;
  
  //ensure that display is at least 1 line by 1 column in size
  _columns = qMax(1,cols);
  _lines = qMax(1,lins);
  _usedColumns = qMin(_usedColumns,_columns);
  _usedLines = qMin(_usedLines,_lines);

  if (_image)
  {
     free(_image);
     makeImage();
  }
  setSize(cols, lins);
  QFrame::setFixedSize(_size);
}

QSize TerminalDisplay::sizeHint() const
{
  return _size;
}

void TerminalDisplay::styleChange(QStyle &)
{
    propagateSize();
}


/* --------------------------------------------------------------------- */
/*                                                                       */
/* Drag & Drop                                                           */
/*                                                                       */
/* --------------------------------------------------------------------- */

void TerminalDisplay::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasFormat("text/plain"))
      event->acceptProposedAction();
}

enum dropPopupOptions { paste, cd, cp, ln, mv };

void TerminalDisplay::dropEvent(QDropEvent* event)
{
   if (_drop==0)
   {
      _drop = new KMenu( this );
      _pasteAction = _drop->addAction( i18n( "Paste" ) );
      _drop->addSeparator();
      _cdAction = _drop->addAction( i18n( "Change Directory" ) );
      _mvAction = _drop->addAction( i18n( "Move Here" ) );
      _cpAction = _drop->addAction( i18n( "Copy Here" ) );
      _lnAction = _drop->addAction( i18n( "Link Here" ) );
      _pasteAction->setData( QVariant( paste ) );
      _cdAction->setData( QVariant( cd ) );
      _mvAction->setData( QVariant( mv ) );
      _cpAction->setData( QVariant( cp ) );
      _lnAction->setData( QVariant( ln ) );
      connect(_drop, SIGNAL(triggered(QAction*)), SLOT(drop_menu_activated(QAction*)));
   };
    // The current behaviour when url(s) are dropped is
    // * if there is only ONE url and if it's a LOCAL one, ask for paste or cd/cp/ln/mv
    // * if there are only LOCAL urls, ask for paste or cp/ln/mv
    // * in all other cases, just paste
    //   (for non-local ones, or for a list of URLs, 'cd' is nonsense)
  _dndFileCount = 0;
  _dropText = "";
  bool justPaste = true;

  KUrl::List urllist = KUrl::List::fromMimeData(event->mimeData());
  if (urllist.count()) {
    justPaste =false;
    KUrl::List::Iterator it;

    _cdAction->setEnabled( true );
    _lnAction->setEnabled( true );

    for ( it = urllist.begin(); it != urllist.end(); ++it ) {
      if(_dndFileCount++ > 0) {
        _dropText += ' ';
        _cdAction->setEnabled( false );
      }
      KUrl url = KIO::NetAccess::mostLocalUrl( *it, 0 );
      QString tmp;
      if (url.isLocalFile()) {
        tmp = url.path(); // local URL : remove protocol. This helps "ln" & "cd" and doesn't harm the others
      } else if ( url.protocol() == QLatin1String( "mailto" ) ) {
        justPaste = true;
        break;
      } else {
        tmp = url.url();
        _cdAction->setEnabled( false );
        _lnAction->setEnabled( false );
      }
      if (urllist.count()>1)
        KRun::shellQuote(tmp);
      _dropText += tmp;
    }

    if (!justPaste) _drop->popup(mapToGlobal(event->pos()));
  }
  if(justPaste && event->mimeData()->hasFormat("text/plain")) {
    kDebug(1211) << "Drop:" << _dropText.toLocal8Bit() << "\n";
    emit sendStringToEmu(_dropText.toLocal8Bit());
    // Paste it
  }
}

void TerminalDisplay::doDrag()
{
  dragInfo.state = diDragging;
  dragInfo.dragObject = new QDrag(this);
  QMimeData *mimeData = new QMimeData;
  mimeData->setText(QApplication::clipboard()->text(QClipboard::Selection));
  dragInfo.dragObject->setMimeData(mimeData);
  dragInfo.dragObject->start(Qt::CopyAction);
  // Don't delete the QTextDrag object.  Qt will delete it when it's done with it.
}

void TerminalDisplay::drop_menu_activated(QAction* action)
{
  int item = action->data().toInt();
  switch (item)
  {
   case paste:
      if (_dndFileCount==1)
        KRun::shellQuote(_dropText);
      emit sendStringToEmu(_dropText.toLocal8Bit());
      activateWindow();
      break;
   case cd:
     emit sendStringToEmu("cd ");
      struct stat statbuf;
      if ( ::stat( QFile::encodeName( _dropText ), &statbuf ) == 0 )
      {
         if ( !S_ISDIR(statbuf.st_mode) )
         {
            KUrl url;
            url.setPath( _dropText );
            _dropText = url.directory( KUrl::ObeyTrailingSlash ); // remove filename
         }
      }
      KRun::shellQuote(_dropText);
      emit sendStringToEmu(_dropText.toLocal8Bit());
      emit sendStringToEmu("\n");
      activateWindow();
      break;
   case cp:
     emit sendStringToEmu("kfmclient copy " );
     break;
   case ln:
     emit sendStringToEmu("ln -s ");
     break;
   case mv:
     emit sendStringToEmu("kfmclient move " );
     break;
   }
   if (item>cd && item<=mv) {
      if (_dndFileCount==1)
        KRun::shellQuote(_dropText);
      emit sendStringToEmu(_dropText.toLocal8Bit());
      emit sendStringToEmu(" .\n");
      activateWindow();
   }
}

void TerminalDisplay::outputSuspended(bool suspended)
{
	//create the label when this function is first called
	if (!_outputSuspendedLabel)
	{
            //This label includes a link to an English language website
            //describing the 'flow control' (Xon/Xoff) feature found in almost all terminal emulators.
            //If there isn't a suitable article available in the target language the link
            //can simply be removed.
			_outputSuspendedLabel = new QLabel( i18n("<qt>Output has been "
                                                "<a href=\"http://en.wikipedia.org/wiki/XON\">suspended</a>"
                                                " by pressing Ctrl+S."
											   "  Press <b>Ctrl+Q</b> to resume.</qt>"),
											   this );

             //fill label with a light yellow 'warning' colour
            //FIXME - It would be better if there was a way of getting a suitable colour based
            //on the current theme.  Last I looked however, the set of colours provided by the theme
            //did not include anything suitable (most being varying shades of grey)

            QPalette palette(_outputSuspendedLabel->palette());
            palette.setColor(QPalette::Base, QColor(255,250,150) );
            _outputSuspendedLabel->setPalette(palette);
			_outputSuspendedLabel->setAutoFillBackground(true);
			_outputSuspendedLabel->setBackgroundRole(QPalette::Base);

            _outputSuspendedLabel->setMargin(5);

            //enable activation of "Xon/Xoff" link in label
            _outputSuspendedLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | 
                                                          Qt::LinksAccessibleByKeyboard);
            _outputSuspendedLabel->setOpenExternalLinks(true);

            _outputSuspendedLabel->setVisible(false);

            _gridLayout->addWidget(_outputSuspendedLabel);       
            _gridLayout->addItem( new QSpacerItem(0,0,QSizePolicy::Expanding,QSizePolicy::Expanding),
                                 1,0);

    }

	_outputSuspendedLabel->setVisible(suspended);
}

uint TerminalDisplay::lineSpacing() const
{
  return _lineSpacing;
}

void TerminalDisplay::setLineSpacing(uint i)
{
  _lineSpacing = i;
  setVTFont(font()); // Trigger an update.
}

#include "TerminalDisplay.moc"