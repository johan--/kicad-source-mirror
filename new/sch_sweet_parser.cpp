/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2011 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2010 Kicad Developers, see change_log.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <sch_sweet_parser.h>
#include <sch_part.h>
#include <sch_lib_table.h>
#include <sch_lpid.h>

#include <macros.h>         // FROM_UTF8()

using namespace SCH;
using namespace PR;


#define MAX_INHERITANCE_NESTING     6       ///< max depth of inheritance, no problem going larger
#define INTERNAL_PER_LOGICAL        10000   ///< no. internal units per logical unit


/**
 * Function log2int
 * converts a logical coordinate to an internal coordinate.  Logical coordinates
 * are defined as the standard distance between pins being equal to one.
 * Internal coordinates are currently INTERNAL_PER_LOGICAL times that.
 */
static inline int log2int( double aCoord )
{
    return int( aCoord * INTERNAL_PER_LOGICAL );
}

static inline int internal( const STRING& aCoord )
{
    return log2int( strtod( aCoord.c_str(), NULL ) );
}


/**
 * Enum PartBit
 * is a set of bit positions that can be used to create flag bits within
 * PART::contains to indicate what state the PART is in and what it contains, i.e.
 * whether the PART has been parsed, and what the PART contains, categorically.
 */
enum PartBit
{
    PARSED,     ///< have parsed this part already, otherwise 'body' text must be parsed
    EXTENDS,    ///< saw "extends" keyword, inheriting from another PART
    VALUE,
    ANCHOR,
    REFERENCE,
    FOOTPRINT,
    DATASHEET,
    MODEL,
    KEYWORDS,
};


/// Function PB
/// is a PartBit shifter for PART::contains field.
static inline const int PB( PartBit oneBitOnly )
{
    return ( 1 << oneBitOnly );
}


void SWEET_PARSER::parseExtends( PART* me )
{
    PART*   base;
    int     offset;

    if( contains & PB(EXTENDS) )
        Duplicate( T_extends );

    NeedSYMBOLorNUMBER();
    me->setExtends( new LPID() );

    offset = me->extends->Parse( CurText() );
    if( offset > -1 )   // -1 is success
        THROW_PARSE_ERROR( _("invalid extends LPID"),
            CurSource(),
            CurLine(),
            CurLineNumber(),
            CurOffset() + offset );

    base = libs->LookupPart( *me->extends, me->Owner() );

    // we could be going in circles here, recursively, or too deep, set limits
    // and disallow extending from self (even indirectly)
    int extendsDepth = 0;
    for( PART* ancestor = base; ancestor && extendsDepth<MAX_INHERITANCE_NESTING;
            ++extendsDepth, ancestor = ancestor->base )
    {
        if( ancestor == me )
        {
            THROW_PARSE_ERROR( _("'extends' may not have self as any ancestor"),
                CurSource(),
                CurLine(),
                CurLineNumber(),
                CurOffset() );
        }
    }

    if( extendsDepth == MAX_INHERITANCE_NESTING )
    {
        THROW_PARSE_ERROR( _("max allowed extends depth exceeded"),
            CurSource(),
            CurLine(),
            CurLineNumber(),
            CurOffset() );
    }

    me->inherit( *base );
    me->base = base;
    contains |= PB(EXTENDS);
}


void SWEET_PARSER::Parse( PART* me, LIB_TABLE* aTable ) throw( IO_ERROR, PARSE_ERROR )
{
    T       tok;

    libs = aTable;

    // empty everything out, could be re-parsing this object and it may not be empty.
    me->clear();

#if 0
    // Be flexible regarding the starting point of the stream.
    // Caller may not have read the first two tokens out of the
    // stream: T_LEFT and T_part, so ignore them if seen here.
    // The 1st two tokens T_LEFT and T_part are then optional in the grammar.
    if( (tok = NextTok() ) == T_LEFT )
    {
        if( ( tok = NextTok() ) != T_part )
            Expecting( T_part );
    }

#else
    // "( part" are not optional
    NeedLEFT();

    if( ( tok = NextTok() ) != T_part )
        Expecting( T_part );
#endif

    NeedSYMBOLorNUMBER();       // toss NAME_HINT
    tok = NextTok();

    // extends must be _first_ thing, if it is present at all, after NAME_HINT
    if( tok == T_extends )
    {
        parseExtends( me );
        tok = NextTok();
    }

    for(  ; tok!=T_RIGHT;  tok = NextTok() )
    {
        if( tok==T_EOF )
            Unexpected( T_EOF );

        if( tok == T_LEFT )
            tok = NextTok();

        // because exceptions are thrown, any 'new' allocation has to be stored
        // somewhere other than on the stack, ASAP.

        switch( tok )
        {
        default:
            // describe what we expect at this level
            Expecting(
                "anchor|value|footprint|model|keywords|alternates\n"
                "|property\n"
                "  |property_del\n"
                "|pin\n"
                "  |pin_merge|pin_swap|pin_renum|pin_rename|route_pin_swap\n"
                "|polyline|line|rectangle|circle|arc|bezier|text"
             );
            break;

        case T_anchor:
            if( contains & PB(ANCHOR) )
                Duplicate( tok );
            NeedNUMBER( "anchor x" );
            me->anchor.x = internal( CurText() );
            NeedNUMBER( "anchor y" );
            me->anchor.y = internal( CurText() );
            contains |= PB(ANCHOR);
            break;

        case T_line:
        case T_polyline:
            POLY_LINE*  pl;
            pl = new POLY_LINE( me );
            me->graphics.push_back( pl );
            parsePolyLine( pl );
            break;

        case T_rectangle:
            RECTANGLE* rect;
            rect = new RECTANGLE( me );
            me->graphics.push_back( rect );
            parseRectangle( rect );
            break;

        case T_circle:
            CIRCLE* circ;
            circ = new CIRCLE( me );
            me->graphics.push_back( circ );
            parseCircle( circ );
            break;

        case T_arc:
            ARC* arc;
            arc = new ARC( me );
            me->graphics.push_back( arc );
            parseArc( arc );
            break;

        case T_bezier:
            BEZIER* bezier;
            bezier = new BEZIER( me );
            me->graphics.push_back( bezier );
            parseBezier( bezier );
            break;

        case T_text:
            GR_TEXT* text;
            text = new GR_TEXT( me );
            me->graphics.push_back( text );
            parseText( text );
            break;

        // reference in a PART is incomplete, it is just the prefix of an
        // unannotated reference. Only components have full reference designators.
        case T_reference:
            if( contains & PB(REFERENCE) )
                Duplicate( tok );
            contains |= PB(REFERENCE);
            NeedSYMBOLorNUMBER();
            me->reference.text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &me->reference.effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_value:
            if( contains & PB(VALUE) )
                Duplicate( tok );
            contains |= PB(VALUE);
            NeedSYMBOLorNUMBER();
            me->value.text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &me->value.effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_footprint:
            if( contains & PB(FOOTPRINT) )
                Duplicate( tok );
            contains |= PB(FOOTPRINT);
            NeedSYMBOLorNUMBER();
            me->footprint.text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &me->footprint.effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_datasheet:
            if( contains & PB(MODEL) )
                Duplicate( tok );
            contains |= PB(MODEL);
            NeedSYMBOLorNUMBER();
            me->datasheet.text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &me->datasheet.effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_model:
            if( contains & PB(MODEL) )
                Duplicate( tok );
            contains |= PB(MODEL);
            NeedSYMBOLorNUMBER();
            me->model.text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &me->model.effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_property:
            PROPERTY* property;
            property = new PROPERTY( me );
            // @todo check for uniqueness
            me->properties.push_back( property );
            NeedSYMBOLorNUMBER();
            property->name = FromUTF8();
            NeedSYMBOLorNUMBER();
            property->text = FromUTF8();
            tok = NextTok();
            if( tok == T_LEFT )
            {
                tok = NextTok();
                if( tok != T_effects )
                    Expecting( T_effects );
                parseTextEffects( &property->effects );
                NeedRIGHT();
            }
            else if( tok != T_RIGHT )
                Expecting( ") | effects" );
            break;

        case T_pin:
            PIN* pin;
            pin = new PIN( me );
            me->pins.push_back( pin );
            parsePin( pin );
            break;


/*
        @todo
        case T_keywords:
            break;

        case T_alternates:
            break;

        case T_property_del:
            break;

        case T_pin_merge:
            break;

        case T_pin_swap:
            break;

        case T_pin_renum:
            break;

        case T_pin_rename:
            break;

        case T_route_pin_swap:
            break;

*/
        }
    }

    contains |= PB(PARSED);

    me->contains |= contains;
}


void SWEET_PARSER::parseFont( FONT* me )
{
    /*
        # The FONT value needs to be defined.  Currently, EESchema does not support
        # different fonts.  In the future this feature may be implemented and at
        # that time FONT will have to be defined.  Initially, only the font size and
        # style are required.  Italic and bold styles are optional.  The font size
        # height and width are in units yet to be determined.
        (font [FONT] (size HEIGHT WIDTH) [italic] [bold])
    */

    // handle the [FONT] 'position dependently', i.e. first
    T       tok = NextTok();
    bool    sawBold   = false;
    bool    sawItalic = false;
    bool    sawSize   = false;

    if( IsSymbol( tok ) )
    {
        me->name = FromUTF8();
        tok = NextTok();
    }

    while( tok != T_RIGHT )
    {
        if( tok == T_LEFT )
        {
            tok = NextTok();

            switch( tok )
            {
            case T_size:
                if( sawSize )
                    Duplicate( T_size );
                sawSize = true;

                NeedNUMBER( "size height" );
                me->size.SetHeight( internal( CurText() ) );

                NeedNUMBER( "size width" );
                me->size.SetWidth( internal( CurText() ) );
                NeedRIGHT();
                break;

            default:
                Expecting( "size" );
            }
        }
        else
        {
            switch( tok )
            {
            case T_bold:
                if( sawBold )
                    Duplicate( T_bold );
                sawBold = true;
                me->bold = true;
                break;

            case T_italic:
                if( sawItalic )
                    Duplicate( T_italic );
                sawItalic = true;
                me->italic = true;
                break;

            default:
                Unexpected( "bold|italic" );
            }
        }

        tok = NextTok();
    }
}


void SWEET_PARSER::parseBool( bool* aBool )
{
    T   tok = NeedSYMBOL();

    switch( tok )
    {
    case T_yes:
    case T_no:
        *aBool = (tok == T_yes);
        break;
    default:
        Expecting( "yes|no" );
    }
}


void SWEET_PARSER::parsePinText( PINTEXT* me )
{
    /*  either:
        (signal SIGNAL   (font [FONT] (size HEIGHT WIDTH) [italic] [bold])(visible YES))
        or
        (padname PADNAME (font [FONT] (size HEIGHT WIDTH) [italic] [bold])(visible YES))
    */
    T       tok;
    bool    sawFont = false;
    bool    sawVis  = false;

    // padname or signal text
    NeedSYMBOLorNUMBER();
    me->text = FromUTF8();

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok == T_LEFT )
        {
            tok = NextTok();

            switch( tok )
            {
            case T_font:
                if( sawFont )
                    Duplicate( tok );
                sawFont = true;
                parseFont( &me->font );
                break;

            case T_visible:
                if( sawVis )
                    Duplicate( tok );
                sawVis = true;
                parseBool( &me->isVisible );
                NeedRIGHT();
                break;

            default:
                Expecting( "font" );
            }
        }

        else
        {
            switch( tok )
            {
            default:
                Expecting( T_LEFT );
            }
        }
    }
}


void SWEET_PARSER::parsePin( PIN* me )
{
    /*
        (pin TYPE SHAPE
            (at X Y [ANGLE])
            (length LENGTH)
            (signal NAME (font [FONT] (size HEIGHT WIDTH) [italic] [bold])(visible YES))
            (padname NUMBER (font [FONT] (size HEIGHT WIDTH) [italic] [bold] (visible YES))
            (visible YES)
        )
    */

    T       tok;
    bool    sawShape   = false;
    bool    sawType    = false;
    bool    sawAt      = false;
    bool    sawLen     = false;
    bool    sawSignal  = false;
    bool    sawPadName = false;
    bool    sawVis     = false;

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok == T_LEFT )
        {
            tok = NextTok();

            switch( tok )
            {
            case T_at:
                if( sawAt )
                    Duplicate( tok );
                sawAt = true;
                parseAt( &me->pos, &me->angle );
                break;

            case T_length:
                if( sawLen )
                    Duplicate( tok );
                sawLen = true;
                NeedNUMBER( "length" );
                me->length = internal( CurText() );
                NeedRIGHT();
                break;

            case T_signal:
                if( sawSignal )
                    Duplicate( tok );
                sawSignal = true;
                parsePinText( &me->signal );
                break;

            case T_padname:
                if( sawPadName )
                    Duplicate( tok );
                sawPadName = true;
                parsePinText( &me->padname );
                break;

            case T_visible:
                if( sawVis )
                    Duplicate( tok );
                sawVis = true;
                parseBool( &me->isVisible );
                NeedRIGHT();
                break;

            default:
                Unexpected( tok );
            }
        }

        else    // not wrapped in parentheses
        {
            switch( tok )
            {
            case T_input:
            case T_output:
            case T_bidirectional:
            case T_tristate:
            case T_passive:
            case T_unspecified:
            case T_power_in:
            case T_power_out:
            case T_open_collector:
            case T_open_emitter:
            case T_unconnected:
                if( sawType )
                    Duplicate( tok );
                sawType = true;
                me->connectionType = tok;
                break;

            case T_none:
            case T_line:
            case T_inverted:
            case T_clock:
            case T_inverted_clk:
            case T_input_low:
            case T_clock_low:
            case T_falling_edge:
            case T_non_logic:
                if( sawShape )
                    Duplicate( tok );
                sawShape = true;
                me->shape = tok;
                break;

            default:
                Unexpected( tok );
            }
        }
    }
}


void SWEET_PARSER::parseTextEffects( TEXT_EFFECTS* me )
{
    /*
        (effects [PROPERTY]

            # Position requires an X and Y coordinates.  Position coordinates can be
            # non-intergr.  Angle is in degrees and defaults to 0 if not defined.
            (at X Y [ANGLE])

            # The FONT value needs to be defined.  Currently, EESchema does not support
            # different fonts.  In the future this feature may be implemented and at
            # that time FONT will have to be defined.  Initially, only the font size and
            # style are required.  Italic and bold styles are optional.  The font size
            # height and width are in units yet to be determined.
            (font [FONT] (size HEIGHT WIDTH) [italic] [bold])

            # Valid visibility values are yes and no.
            (visible YES)
        )
    */

    bool    sawFont = false;
    bool    sawAt   = false;
    bool    sawVis  = false;

    T       tok = NextTok();

    if( IsSymbol( tok ) )
    {
        me->propName = FromUTF8();
        tok = NextTok();
    }

    while( tok != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_at:
            if( sawAt )
                Duplicate( tok );
            sawAt = true;
            parseAt( &me->pos, &me->angle );
            break;

        case T_font:
            if( sawFont )
                Duplicate( tok );
            sawFont = true;
            parseFont( &me->font );
            break;

        case T_visible:
            if( sawVis )
                Duplicate( sawVis );
            sawVis = true;
            parseBool( &me->isVisible );
            NeedRIGHT();
            break;

        default:
            Expecting( "at|font|visible" );
        }

        tok = NextTok();
    }
}


void SWEET_PARSER::parsePolyLine( POLY_LINE* me )
{
    /*
        (polyline|line
            (pts (xy X Y) (xy X Y) (xy X Y) (xy X Y) (xy X Y))

            # Line widths are in units as defined above.
            (line_width WIDTH)

            # Valid fill types are none, filled, and transparent.
            (fill FILL_TYPE)
        )
    */

    T       tok;
    int     count = 0;
    bool    sawWidth = false;
    bool    sawFill  = false;

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_line_width:
            if( sawWidth )
                Duplicate( tok );
            NeedNUMBER( "line_width" );
            me->lineWidth = strtod( CurText(), NULL );
            NeedRIGHT();
            sawWidth = true;
            break;

        case T_pts:
            if( count )
                Duplicate( tok );
            for(  ;  ( tok = NextTok() ) != T_RIGHT;  ++count )
            {
                if( tok != T_LEFT )
                    Expecting( T_LEFT );

                tok = NeedSYMBOL();
                if( tok != T_xy )
                    Expecting( T_xy );

                me->pts.push_back( POINT() );

                NeedNUMBER( "x" );
                me->pts.back().x = internal( CurText() );

                NeedNUMBER( "y" );
                me->pts.back().y = internal( CurText() );

                NeedRIGHT();
            }
            if( count < 2 )
                Expecting( ">= 2 pts" );
            break;

        case T_fill:
            if( sawFill )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_none:
            case T_filled:
            case T_transparent:
                me->fillType = tok;
                break;
            default:
                Expecting( "none|filled|transparent" );
            }
            NeedRIGHT();
            sawFill = true;
            break;

        default:
            Expecting( "pts|line_width|fill" );
        }
    }
}


void SWEET_PARSER::parseBezier( BEZIER* me )
{
    parsePolyLine( me );
}


void SWEET_PARSER::parseRectangle( RECTANGLE* me )
{
    /*
        (rectangle (start X Y) (end X Y) (line_width WIDTH) (fill FILL_TYPE))
    */

    T       tok;
    bool    sawStart = false;
    bool    sawEnd   = false;
    bool    sawWidth = false;
    bool    sawFill  = false;

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_line_width:
            if( sawWidth )
                Duplicate( tok );
            NeedNUMBER( "line_width" );
            me->lineWidth = strtod( CurText(), NULL );
            NeedRIGHT();
            sawWidth = true;
            break;

        case T_fill:
            if( sawFill )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_none:
            case T_filled:
            case T_transparent:
                me->fillType = tok;
                break;
            default:
                Expecting( "none|filled|transparent" );
            }
            NeedRIGHT();
            sawFill = true;
            break;

        case T_start:
            if( sawStart )
                Duplicate( tok );
            NeedNUMBER( "x" );
            me->start.x = internal( CurText() );
            NeedNUMBER( "y" );
            me->start.y = internal( CurText() );
            NeedRIGHT();
            sawStart = true;
            break;

        case T_end:
            if( sawEnd )
                Duplicate( tok );
            NeedNUMBER( "x" );
            me->end.x = internal( CurText() );
            NeedNUMBER( "y" );
            me->end.y = internal( CurText() );
            NeedRIGHT();
            sawEnd = true;
            break;

        default:
            Expecting( "start|end|line_width|fill" );
        }
    }
}


void SWEET_PARSER::parseCircle( CIRCLE* me )
{
    /*
        (circle (center X Y)
            # Radius length is in units if defined or mils.
            (radius LENGTH)
            (line_width WIDTH)
            (fill FILL_TYPE)
        )
    */

    T       tok;
    bool    sawCenter = false;
    bool    sawRadius = false;
    bool    sawWidth  = false;
    bool    sawFill   = false;

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_line_width:
            if( sawWidth )
                Duplicate( tok );
            NeedNUMBER( "line_width" );
            me->lineWidth = strtod( CurText(), NULL );
            NeedRIGHT();
            sawWidth = true;
            break;

        case T_fill:
            if( sawFill )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_none:
            case T_filled:
            case T_transparent:
                me->fillType = tok;
                break;
            default:
                Expecting( "none|filled|transparent" );
            }
            NeedRIGHT();
            sawFill = true;
            break;

        case T_center:
            if( sawCenter )
                Duplicate( tok );
            NeedNUMBER( "center x" );
            me->center.x = internal( CurText() );
            NeedNUMBER( "center y" );
            me->center.y = internal( CurText() );
            NeedRIGHT();
            sawCenter = true;
            break;

        case T_radius:
            if( sawRadius )
                Duplicate( tok );
            NeedNUMBER( "radius" );
            me->radius = internal( CurText() );
            NeedRIGHT();
            sawRadius = true;
            break;

        default:
            Expecting( "center|radius|line_width|fill" );
        }
    }
}


void SWEET_PARSER::parseArc( ARC* me )
{
    /*
        (arc (pos X Y) (radius RADIUS) (start X Y) (end X Y)
            (line_width WIDTH)
            (fill FILL_TYPE)
        )
    */

    T       tok;
    bool    sawPos    = false;
    bool    sawStart  = false;
    bool    sawEnd    = false;
    bool    sawRadius = false;
    bool    sawWidth  = false;
    bool    sawFill   = false;

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_line_width:
            if( sawWidth )
                Duplicate( tok );
            NeedNUMBER( "line_width" );
            me->lineWidth = strtod( CurText(), NULL );
            NeedRIGHT();
            sawWidth = true;
            break;

        case T_fill:
            if( sawFill )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_none:
            case T_filled:
            case T_transparent:
                me->fillType = tok;
                break;
            default:
                Expecting( "none|filled|transparent" );
            }
            NeedRIGHT();
            sawFill = true;
            break;

        case T_pos:
            if( sawPos )
                Duplicate( tok );
            NeedNUMBER( "pos x" );
            me->pos.x = internal( CurText() );
            NeedNUMBER( "pos y" );
            me->pos.y = internal( CurText() );
            NeedRIGHT();
            sawPos = true;
            break;

        case T_radius:
            if( sawRadius )
                Duplicate( tok );
            NeedNUMBER( "radius" );
            me->radius = internal( CurText() );
            NeedRIGHT();
            sawRadius = true;
            break;

        case T_start:
            if( sawStart )
                Duplicate( tok );
            NeedNUMBER( "start x" );
            me->start.x = internal( CurText() );
            NeedNUMBER( "start y" );
            me->start.y = internal( CurText() );
            NeedRIGHT();
            sawStart = true;
            break;

        case T_end:
            if( sawEnd )
                Duplicate( tok );
            NeedNUMBER( "end x" );
            me->end.x = internal( CurText() );
            NeedNUMBER( "end y" );
            me->end.y = internal( CurText() );
            NeedRIGHT();
            sawEnd = true;
            break;

        default:
            Expecting( "center|radius|line_width|fill" );
        }
    }
}


void SWEET_PARSER::parseAt( POINT* pos, float* angle )
{
    T       tok;

    NeedNUMBER( "at x" );
    pos->x = internal( CurText() );

    NeedNUMBER( "at y" );
    pos->y = internal( CurText() );

    tok = NextTok();
    if( angle && tok == T_NUMBER )
    {
        *angle = strtod( CurText(), NULL );
        tok = NextTok();
    }
    if( tok != T_RIGHT )
        Expecting( T_RIGHT );
}


void SWEET_PARSER::parseText( GR_TEXT* me )
{
    /*
        (text "This is the text that gets drawn."
            (at X Y [ANGLE])

            # Valid horizontal justification values are center, right, and left.  Valid
            # vertical justification values are center, top, bottom.
            (justify HORIZONTAL_JUSTIFY VERTICAL_JUSTIFY)
            (font [FONT] (size HEIGHT WIDTH) [italic] [bold])
            (visible YES)
            (fill FILL_TYPE)
        )
    */

    T       tok;
    bool    sawAt   = false;
    bool    sawFill = false;
    bool    sawFont = false;
    bool    sawVis  = false;
    bool    sawJust = false;

    NeedSYMBOLorNUMBER();
    me->text = FROM_UTF8( CurText() );

    while( ( tok = NextTok() ) != T_RIGHT )
    {
        if( tok != T_LEFT )
            Expecting( T_LEFT );

        tok = NextTok();

        switch( tok )
        {
        case T_at:
            if( sawAt )
                Duplicate( tok );
            parseAt( &me->pos, &me->angle );
            sawAt = true;
            break;

        case T_fill:
            if( sawFill )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_none:
            case T_filled:
            case T_transparent:
                me->fillType = tok;
                break;
            default:
                Expecting( "none|filled|transparent" );
            }
            NeedRIGHT();
            sawFill = true;
            break;

        case T_justify:
            if( sawJust )
                Duplicate( tok );
            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_center:
            case T_right:
            case T_left:
                me->hjustify = tok;
                break;
            default:
                Expecting( "center|right|left" );
            }

            tok = NeedSYMBOL();
            switch( tok )
            {
            case T_center:
            case T_top:
            case T_bottom:
                me->vjustify = tok;
                break;
            default:
                Expecting( "center|top|bottom" );
            }
            NeedRIGHT();
            sawJust = true;
            break;

        case T_visible:
            if( sawVis )
                Duplicate( tok );
            parseBool( &me->isVisible );
            NeedRIGHT();
            sawVis = true;
            break;

        case T_font:
            if( sawFont )
                Duplicate( tok );
            sawFont = true;
            parseFont( &me->font );
            break;

        default:
            Expecting( "at|justify|font|visible|fill" );
        }
    }
}

