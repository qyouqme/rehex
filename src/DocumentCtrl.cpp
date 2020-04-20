/* Reverse Engineer's Hex Editor
 * Copyright (C) 2017-2020 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <algorithm>
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <iterator>
#include <jansson.h>
#include <limits>
#include <map>
#include <stack>
#include <string>
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>

#include "app.hpp"
#include "document.hpp"
#include "DocumentCtrl.hpp"
#include "Events.hpp"
#include "Palette.hpp"
#include "textentrydialog.hpp"
#include "util.hpp"

static_assert(std::numeric_limits<json_int_t>::max() >= std::numeric_limits<off_t>::max(),
	"json_int_t must be large enough to store any offset in an off_t");

/* Is the given byte a printable 7-bit ASCII character? */
static bool isasciiprint(int c)
{
	return (c >= ' ' && c <= '~');
}

/* Is the given value a 7-bit ASCII character representing a hex digit? */
static bool isasciihex(int c)
{
	return (c >= '0' && c <= '9')
		|| (c >= 'A' && c <= 'F')
		|| (c >= 'a' && c <= 'f');
}

enum {
	ID_REDRAW_CURSOR = 1,
	ID_SELECT_TIMER,
};

BEGIN_EVENT_TABLE(REHex::DocumentCtrl, wxControl)
	EVT_PAINT(REHex::DocumentCtrl::OnPaint)
	EVT_SIZE(REHex::DocumentCtrl::OnSize)
	EVT_SCROLLWIN(REHex::DocumentCtrl::OnScroll)
	EVT_MOUSEWHEEL(REHex::DocumentCtrl::OnWheel)
	EVT_CHAR(REHex::DocumentCtrl::OnChar)
	EVT_LEFT_DOWN(REHex::DocumentCtrl::OnLeftDown)
	EVT_LEFT_UP(REHex::DocumentCtrl::OnLeftUp)
	EVT_RIGHT_DOWN(REHex::DocumentCtrl::OnRightDown)
	EVT_MOTION(REHex::DocumentCtrl::OnMotion)
	EVT_TIMER(ID_SELECT_TIMER, REHex::DocumentCtrl::OnSelectTick)
	EVT_TIMER(ID_REDRAW_CURSOR, REHex::DocumentCtrl::OnRedrawCursor)
END_EVENT_TABLE()

REHex::DocumentCtrl::DocumentCtrl(wxWindow *parent, REHex::Document *doc):
	wxControl(),
	doc(doc),
	redraw_cursor_timer(this, ID_REDRAW_CURSOR),
	mouse_select_timer(this, ID_SELECT_TIMER)
{
	/* The background style MUST be set before the control is created. */
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
		(wxVSCROLL | wxHSCROLL | wxWANTS_CHARS));
	
	client_width      = 0;
	client_height     = 0;
	visible_lines     = 1;
	bytes_per_line    = 0;
	bytes_per_group   = 4;
	offset_display_base = OFFSET_BASE_HEX;
	show_ascii        = true;
	inline_comment_mode = ICM_FULL_INDENT;
	highlight_selection_match = false;
	scroll_xoff       = 0;
	scroll_yoff       = 0;
	scroll_yoff_max   = 0;
	scroll_ydiv       = 1;
	wheel_vert_accum  = 0;
	wheel_horiz_accum = 0;
	selection_off     = 0;
	selection_length  = 0;
	cursor_visible    = true;
	mouse_down_in_hex = false;
	mouse_down_in_ascii = false;
	mouse_shift_initial = -1;
	cursor_state      = CSTATE_HEX;
	
	wxFontInfo finfo;
	finfo.Family(wxFONTFAMILY_MODERN);
	
	hex_font = new wxFont(finfo);
	assert(hex_font->IsFixedWidth());
	
	{
		wxClientDC dc(this);
		dc.SetFont(*hex_font);
		
		wxSize hf_char_size = dc.GetTextExtent("X");
		hf_height           = hf_char_size.GetHeight();
		
		/* Precompute widths for hf_string_width() */
		
		for(unsigned int i = 0; i < PRECOMP_HF_STRING_WIDTH_TO; ++i)
		{
			hf_string_width_precomp[i]
				= dc.GetTextExtent(std::string((i + 1), 'X')).GetWidth();
		}
	}
	
	redraw_cursor_timer.Start(750, wxTIMER_CONTINUOUS);
	
	/* SetDoubleBuffered() isn't implemented on all platforms. */
	#if defined(__WXMSW__) || defined(__WXGTK__)
	SetDoubleBuffered(true);
	#endif
	
	SetMinClientSize(wxSize(hf_string_width(60), (hf_height * 20)));
}

REHex::DocumentCtrl::~DocumentCtrl()
{
	for(auto region = regions.begin(); region != regions.end(); ++region)
	{
		delete *region;
	}
}

unsigned int REHex::DocumentCtrl::get_bytes_per_line()
{
	return bytes_per_line;
}

void REHex::DocumentCtrl::set_bytes_per_line(unsigned int bytes_per_line)
{
	this->bytes_per_line = bytes_per_line;
	_handle_width_change();
}

unsigned int REHex::DocumentCtrl::get_bytes_per_group()
{
	return bytes_per_group;
}

void REHex::DocumentCtrl::set_bytes_per_group(unsigned int bytes_per_group)
{
	this->bytes_per_group = bytes_per_group;
	_handle_width_change();
}

bool REHex::DocumentCtrl::get_show_offsets()
{
	return offset_column;
}

void REHex::DocumentCtrl::set_show_offsets(bool show_offsets)
{
	offset_column = show_offsets;
	_handle_width_change();
}

REHex::OffsetBase REHex::DocumentCtrl::get_offset_display_base() const
{
	return offset_display_base;
}

void REHex::DocumentCtrl::set_offset_display_base(REHex::OffsetBase offset_display_base)
{
	this->offset_display_base = offset_display_base;
	_handle_width_change();
	
	wxCommandEvent event(REHex::EV_BASE_CHANGED);
	event.SetEventObject(this);
	
	wxPostEvent(this, event);
}

bool REHex::DocumentCtrl::get_show_ascii()
{
	return show_ascii;
}

void REHex::DocumentCtrl::set_show_ascii(bool show_ascii)
{
	this->show_ascii = show_ascii;
	_handle_width_change();
}

off_t REHex::DocumentCtrl::get_cursor_position() const
{
	return this->cpos_off;
}

void REHex::DocumentCtrl::set_cursor_position(off_t off)
{
	_set_cursor_position(off, CSTATE_GOTO);
}

void REHex::DocumentCtrl::_set_cursor_position(off_t position, enum CursorState cursor_state)
{
	assert(position >= 0 && position <= doc->buffer_length());
	
	if(!insert_mode && position > 0 && position == doc->buffer_length())
	{
		--position;
	}
	
	if(cursor_state == CSTATE_GOTO)
	{
		if(this->cursor_state == CSTATE_HEX_MID)
		{
			cursor_state = CSTATE_HEX;
		}
		else{
			cursor_state = this->cursor_state;
		}
	}
	
	/* Blink cursor to visibility and reset timer */
	cursor_visible = true;
	redraw_cursor_timer.Start();
	
	bool cursor_moved = (cpos_off != position);
	
	cpos_off = position;
	this->cursor_state = cursor_state;
	
	_make_byte_visible(cpos_off);
	
	if(cursor_moved)
	{
		// TODO
		// _raise_moved();
	}
	
	/* TODO: Limit paint to affected area */
	Refresh();
}

bool REHex::DocumentCtrl::get_insert_mode()
{
	return this->insert_mode;
}

void REHex::DocumentCtrl::set_insert_mode(bool enabled)
{
	if(insert_mode == enabled)
	{
		return;
	}
	
	insert_mode = enabled;
	
	off_t cursor_pos = get_cursor_position();
	if(!insert_mode && cursor_pos > 0 && cursor_pos == doc->buffer_length())
	{
		/* Move cursor back if going from insert to overwrite mode and it
		 * was at the end of the file.
		*/
		set_cursor_position(cursor_pos - 1);
	}
	
	wxCommandEvent event(REHex::EV_INSERT_TOGGLED);
	event.SetEventObject(this);
	wxPostEvent(this, event);
	
	/* TODO: Limit paint to affected area */
	this->Refresh();
}

void REHex::DocumentCtrl::set_selection(off_t off, off_t length)
{
	selection_off    = off;
	selection_length = length;
	
	if(length <= 0 || mouse_shift_initial < off || mouse_shift_initial > (off + length))
	{
		mouse_shift_initial = -1;
	}
	
	{
		wxCommandEvent event(REHex::EV_SELECTION_CHANGED);
		event.SetEventObject(this);
		
		wxPostEvent(this, event);
	}
	
	/* TODO: Limit paint to affected area */
	Refresh();
}

void REHex::DocumentCtrl::clear_selection()
{
	set_selection(0, 0);
}

std::pair<off_t, off_t> REHex::DocumentCtrl::get_selection()
{
	return std::make_pair(selection_off, selection_length);
}

void REHex::DocumentCtrl::OnPaint(wxPaintEvent &event)
{
	wxBufferedPaintDC dc(this);
	
	dc.SetFont(*hex_font);
	
	dc.SetBackground(wxBrush((*active_palette)[Palette::PAL_NORMAL_TEXT_BG]));
	dc.Clear();
	
	/* Iterate over the regions to find the last one which does NOT start beyond the current
	 * scroll_y.
	*/
	
	auto region = regions.begin();
	for(auto next = std::next(region); next != regions.end() && (*next)->y_offset <= scroll_yoff; ++next)
	{
		region = next;
	}
	
	int64_t yo_end = scroll_yoff + visible_lines + 1;
	for(; region != regions.end() && (*region)->y_offset < yo_end; ++region)
	{
		int x_px = 0 - scroll_xoff;
		
		int64_t y_px = (*region)->y_offset;
		assert(y_px >= 0);
		
		y_px -= scroll_yoff;
		y_px *= hf_height;
		
		(*region)->draw(*this, dc, x_px, y_px);
	}
}

void REHex::DocumentCtrl::OnSize(wxSizeEvent &event)
{
	if(regions.empty())
	{
		/* Great big dirty hack: If regions is empty, we're being invoked within the
		 * Create() method call and we aren't set up properly yet, do nothing.
		*/
		return;
	}
	
	/* Get the size of the area we can draw into */
	
	wxSize client_size    = GetClientSize();
	int new_client_width  = client_size.GetWidth();
	int new_client_height = client_size.GetHeight();
	
	bool width_changed  = (new_client_width  != client_width);
	bool height_changed = (new_client_height != client_height);
	
	client_width  = new_client_width;
	client_height = new_client_height;
	
	/* Clamp to 1 if window is too small to display a single whole line, to avoid edge casey
	 * crashing in the scrolling code.
	*/
	visible_lines = std::max((client_height / hf_height), 1);
	
	if(width_changed)
	{
		_handle_width_change();
	}
	else if(height_changed)
	{
		/* _handle_height_change() is a subset of _handle_width_change() */
		_handle_height_change();
	}
}

void REHex::DocumentCtrl::_handle_width_change()
{
	/* Calculate how much space (if any) to reserve for the offsets to the left. */
	
	if(offset_column)
	{
		/* Offset column width includes the vertical line between it and the hex area, so
		 * size is calculated for n+1 characters.
		*/
		
		if(doc->buffer_length() > 0xFFFFFFFF)
		{
			if(offset_display_base == OFFSET_BASE_HEX)
			{
				offset_column_width = hf_string_width(18);
			}
			else{
				offset_column_width = hf_string_width(20);
			}
		}
		else{
			if(offset_display_base == OFFSET_BASE_HEX)
			{
				offset_column_width = hf_string_width(10);
			}
			else{
				offset_column_width = hf_string_width(11);
			}
		}
	}
	else{
		offset_column_width = 0;
	}
	
	auto calc_row_width = [this](unsigned int line_bytes, const REHex::DocumentCtrl::DataRegion *dr)
	{
		return offset_column_width
			/* indentation */
			+ (_indent_width(dr->indent_depth) * 2)
			
			/* hex data */
			+ hf_string_width(line_bytes * 2)
			+ hf_string_width((line_bytes - 1) / bytes_per_group)
			
			/* ASCII data */
			+ (show_ascii * hf_char_width())
			+ (show_ascii * hf_string_width(line_bytes));
	};
	
	virtual_width = 0;
	
	for(auto r = regions.begin(); r != regions.end(); ++r)
	{
		REHex::DocumentCtrl::DataRegion *dr = dynamic_cast<REHex::DocumentCtrl::DataRegion*>(*r);
		if(dr != NULL)
		{
			/* Decide how many bytes to display per line */
			
			if(bytes_per_line == 0) /* 0 is "as many as will fit in the window" */
			{
				/* TODO: Can I do this algorithmically? */
				
				dr->bytes_per_line_actual = 1;
				
				while(calc_row_width((dr->bytes_per_line_actual + 1), dr) <= client_width)
				{
					++(dr->bytes_per_line_actual);
				}
			}
			else{
				dr->bytes_per_line_actual = bytes_per_line;
			}
			
			int dr_min_width = calc_row_width(dr->bytes_per_line_actual, dr);
			if(dr_min_width > virtual_width)
			{
				virtual_width = dr_min_width;
			}
		}
	}
	
	if(virtual_width < client_width)
	{
		/* Raise virtual_width to client_width, so that things drawn relative to the right
		 * edge of the virtual client area don't end up in the middle.
		*/
		virtual_width = client_width;
	}
	
	/* TODO: Preserve/scale the position as the window size changes. */
	SetScrollbar(wxHORIZONTAL, 0, client_width, virtual_width);
	
	/* Recalculate the height and y offset of each region. */
	
	{
		wxClientDC dc(this);
		_recalc_regions(dc);
	}
	
	/* Update vertical scrollbar, since we just recalculated the height of the document. */
	_update_vscroll();
	
	/* Force a redraw of the whole control since resizing can change pretty much the entire
	 * thing depending on rendering settings.
	*/
	Refresh();
}

void REHex::DocumentCtrl::_handle_height_change()
{
	/* Update vertical scrollbar, since the client area height has changed. */
	_update_vscroll();
	
	/* Force a redraw of the whole control since resizing can change pretty much the entire
	 * thing depending on rendering settings.
	*/
	Refresh();
}

void REHex::DocumentCtrl::_update_vscroll()
{
	static const int MAX_STEPS = 10000;
	
	uint64_t total_lines = regions.back()->y_offset + regions.back()->y_lines;
	
	if(total_lines > visible_lines)
	{
		int64_t new_scroll_yoff_max = total_lines - visible_lines;
		
		/* Try to keep the vertical scroll position at roughly the same point in the file. */
		scroll_yoff = (scroll_yoff > 0)
			? ((double)(scroll_yoff) * ((double)(new_scroll_yoff_max) / (double)(scroll_yoff_max)))
			: 0;
		
		/* In case of rounding errors. */
		if(scroll_yoff > scroll_yoff_max)
		{
			scroll_yoff = scroll_yoff_max;
		}
		else if(scroll_yoff < 0)
		{
			scroll_yoff = 0;
		}
		
		int range, thumb, position;
		
		if(total_lines <= (uint64_t)(MAX_STEPS))
		{
			scroll_ydiv = 1;
			
			range    = total_lines;
			thumb    = visible_lines;
			position = scroll_yoff;
		}
		else{
			scroll_ydiv = total_lines / MAX_STEPS;
			
			range    = MAX_STEPS;
			thumb    = 1;
			position = std::min((int)(scroll_yoff / scroll_ydiv), (range - thumb));
			
			if(position == 0 && scroll_yoff > 0)
			{
				/* Past the first line, but not the first scrollbar division.
				 * Skip to the next so the scrollbar doesn't appear fully scrolled
				 * up when there's a bit to go.
				*/
				position = 1;
			}
			else if(position == (range - thumb) && scroll_yoff < scroll_yoff_max)
			{
				/* Ditto, but for the bottom of the document. */
				--position;
			}
		}
		
		assert(range > 0);
		assert(range <= MAX_STEPS);
		assert(thumb > 0);
		assert(thumb <= range);
		assert(position >= 0);
		assert(position <= (range - thumb));
		
		SetScrollbar(wxVERTICAL, position, thumb, range);
		scroll_yoff_max = new_scroll_yoff_max;
	}
	else{
		/* We don't need a vertical scroll bar, but force one to appear anyway so
		 * the bytes per line can't change within OnSize and get us stuck in a loop.
		*/
		#ifdef _WIN32
		SetScrollbar(wxVERTICAL, 0, 0, -1);
		#else
		/* TODO: Do this in a non-crappy way on non-win32 */
		SetScrollbar(wxVERTICAL, 0, 1, 2);
		#endif
		
		scroll_yoff_max = 0;
	}
}

void REHex::DocumentCtrl::_update_vscroll_pos()
{
	int range = GetScrollRange(wxVERTICAL);
	int thumb = GetScrollThumb(wxVERTICAL);
	
	if(scroll_yoff == scroll_yoff_max)
	{
		/* Last line, overcome any rounding and set scroll bar to max. */
		SetScrollPos(wxVERTICAL, (range - thumb));
	}
	else{
		int position = std::min((int)(scroll_yoff / scroll_ydiv), (range - thumb));
		if(position == 0 && scroll_yoff > 0)
		{
			/* Past the first line, but not the first scrollbar division.
			 * Skip to the next so the scrollbar doesn't appear fully scrolled
			 * up when there's a bit to go.
			*/
			position = 1;
		}
		else if(position == (range - thumb) && scroll_yoff < scroll_yoff_max)
		{
			/* Ditto, but for the bottom of the document. */
			--position;
		}
		
		assert(position >= 0);
		assert(position <= (range - thumb));
		
		SetScrollPos(wxVERTICAL, position);
	}
}

void REHex::DocumentCtrl::OnScroll(wxScrollWinEvent &event)
{
	wxEventType type = event.GetEventType();
	int orientation  = event.GetOrientation();
	
	if(orientation == wxVERTICAL)
	{
		if(type == wxEVT_SCROLLWIN_THUMBTRACK || type == wxEVT_SCROLLWIN_THUMBRELEASE)
		{
			int position = event.GetPosition();
			int range = GetScrollRange(wxVERTICAL);
			int thumb = GetScrollThumb(wxVERTICAL);
			
			if(position == (range - thumb))
			{
				/* Dragged to the end of the scroll bar, jump to last line. */
				scroll_yoff = scroll_yoff_max;
			}
			else{
				scroll_yoff = position * scroll_ydiv;
			}
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_TOP)
		{
			scroll_yoff = 0;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_BOTTOM)
		{
			scroll_yoff = scroll_yoff_max;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEUP)
		{
			--scroll_yoff;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEDOWN)
		{
			++scroll_yoff;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEUP)
		{
			scroll_yoff -= visible_lines;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEDOWN)
		{
			scroll_yoff += visible_lines;
		}
		
		if(scroll_yoff < 0)
		{
			scroll_yoff = 0;
		}
		else if(scroll_yoff > scroll_yoff_max)
		{
			scroll_yoff = scroll_yoff_max;
		}
		
		_update_vscroll_pos();
		Refresh();
	}
	else if(orientation == wxHORIZONTAL)
	{
		if(type == wxEVT_SCROLLWIN_THUMBTRACK || type == wxEVT_SCROLLWIN_THUMBRELEASE)
		{
			scroll_xoff = event.GetPosition();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_TOP)
		{
			scroll_xoff = 0;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_BOTTOM)
		{
			scroll_xoff = virtual_width - client_width;
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEUP)
		{
			scroll_xoff -= hf_char_width();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_LINEDOWN)
		{
			scroll_xoff += hf_char_width();
		}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEUP)   {}
		else if(event.GetEventType() == wxEVT_SCROLLWIN_PAGEDOWN) {}
		
		if(scroll_xoff < 0)
		{
			scroll_xoff = 0;
		}
		else if(scroll_xoff > (virtual_width - client_width))
		{
			scroll_xoff = virtual_width - client_width;
		}
		
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		Refresh();
	}
}

void REHex::DocumentCtrl::OnWheel(wxMouseEvent &event)
{
	wxMouseWheelAxis axis = event.GetWheelAxis();
	int delta             = event.GetWheelDelta();
	int ticks_per_delta   = event.GetLinesPerAction();
	
	if(axis == wxMOUSE_WHEEL_VERTICAL)
	{
		wheel_vert_accum += event.GetWheelRotation();
		
		scroll_yoff -= (wheel_vert_accum / delta) * ticks_per_delta;
		
		wheel_vert_accum = (wheel_vert_accum % delta);
		
		if(scroll_yoff < 0)
		{
			scroll_yoff = 0;
		}
		else if(scroll_yoff > scroll_yoff_max)
		{
			scroll_yoff = scroll_yoff_max;
		}
		
		_update_vscroll_pos();
		Refresh();
	}
	else if(axis == wxMOUSE_WHEEL_HORIZONTAL)
	{
		ticks_per_delta *= hf_char_width();
		
		wheel_horiz_accum += event.GetWheelRotation();
		
		scroll_xoff += (wheel_horiz_accum / delta) * ticks_per_delta;
		
		wheel_horiz_accum = (wheel_horiz_accum % delta);
		
		if(scroll_xoff < 0)
		{
			scroll_xoff = 0;
		}
		else if(scroll_xoff > (virtual_width - client_width))
		{
			scroll_xoff = virtual_width - client_width;
		}
		
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		Refresh();
	}
}

void REHex::DocumentCtrl::OnChar(wxKeyEvent &event)
{
	int key       = event.GetKeyCode();
	int modifiers = event.GetModifiers();
	
	off_t cursor_pos = get_cursor_position();
	
	if(key == WXK_TAB && modifiers == wxMOD_NONE)
	{
		if(cursor_state != CSTATE_ASCII)
		{
			/* Hex view is focused, focus the ASCII view. */
			
			cursor_state = CSTATE_ASCII;
			Refresh();
		}
		else{
			/* ASCII view is focused, get wxWidgets to process this and focus the next
			 * control in the window.
			*/
			
			HandleAsNavigationKey(event);
		}
		
		return;
	}
	else if(key == WXK_TAB && modifiers == wxMOD_SHIFT)
	{
		if(cursor_state == CSTATE_ASCII)
		{
			/* ASCII view is focused, focus the hex view. */
			
			cursor_state = CSTATE_HEX;
			Refresh();
		}
		else{
			/* Hex view is focused, get wxWidgets to process this and focus the previous
			 * control in the window.
			*/
			
			HandleAsNavigationKey(event);
		}
	}
	else if((modifiers == wxMOD_NONE || modifiers == wxMOD_SHIFT || ((modifiers & ~wxMOD_SHIFT) == wxMOD_CONTROL && (key == WXK_HOME || key == WXK_END)))
		&& (key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN || key == WXK_HOME || key == WXK_END))
	{
		off_t new_cursor_pos = cursor_pos;
		
		if(key == WXK_LEFT)
		{
			new_cursor_pos = cursor_pos - (cursor_pos > 0);
		}
		else if(key == WXK_RIGHT)
		{
			off_t max_pos = std::max((doc->buffer_length() - !get_insert_mode()), (off_t)(0));
			new_cursor_pos = std::min((cursor_pos + 1), max_pos);
		}
		else if(key == WXK_UP)
		{
			auto cur_region = _data_region_by_offset(cursor_pos);
			assert(cur_region != NULL);
			
			off_t offset_within_cur = cursor_pos - cur_region->d_offset;
			
			if(offset_within_cur >= cur_region->bytes_per_line_actual)
			{
				/* We are at least on the second line of the current
				 * region, can jump to the previous one.
				*/
				new_cursor_pos = cursor_pos - cur_region->bytes_per_line_actual;
			}
			else if(cur_region->d_offset > 0)
			{
				/* We are on the first line of the current region, but there is at
				 * last one region before us.
				*/
				auto prev_region = _data_region_by_offset(cur_region->d_offset - 1);
				assert(prev_region != NULL);
				
				/* How many bytes on the last line of prev_region? */
				off_t pr_last_line_len = (prev_region->d_length % prev_region->bytes_per_line_actual);
				if(pr_last_line_len == 0)
				{
					pr_last_line_len = prev_region->bytes_per_line_actual;
				}
				
				if(pr_last_line_len > offset_within_cur)
				{
					/* The last line of the previous block is at least long
					 * enough to have a byte above the current cursor position
					 * on the screen.
					*/
					
					new_cursor_pos = (cursor_pos - offset_within_cur) - (pr_last_line_len - offset_within_cur);
				}
				else{
					/* The last line of the previous block falls short of the
					 * horizontal position of the cursor, just jump to the end
					 * of it.
					*/
					
					new_cursor_pos = cur_region->d_offset - 1;
				}
			}
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		else if(key == WXK_DOWN)
		{
			auto cur_region = _data_region_by_offset(cursor_pos);
			assert(cur_region != NULL);
			
			off_t offset_within_cur = cursor_pos - cur_region->d_offset;
			off_t remain_within_cur = cur_region->d_length - offset_within_cur;
			
			off_t last_line_within_cur = cur_region->d_length
				- (((cur_region->d_length % cur_region->bytes_per_line_actual) == 0)
					? cur_region->bytes_per_line_actual
					: (cur_region->d_length % cur_region->bytes_per_line_actual));
			
			if(remain_within_cur > cur_region->bytes_per_line_actual)
			{
				/* There is at least one more line's worth of bytes in the
				 * current region, can just skip ahead.
				*/
				new_cursor_pos = cursor_pos + cur_region->bytes_per_line_actual;
			}
			else if(offset_within_cur < last_line_within_cur)
			{
				/* There is another line in the current region which falls short of
				 * the cursor's horizontal position, jump to its end.
				*/
				new_cursor_pos = cur_region->d_offset + cur_region->d_length - 1;
			}
			else{
				auto next_region = _data_region_by_offset(cur_region->d_offset + cur_region->d_length);
				
				if(next_region != NULL && cur_region != next_region)
				{
					/* There is another region after this one, jump to the same
					 * it, offset by our offset in the current line.
					*/
					new_cursor_pos = next_region->d_offset + (offset_within_cur % cur_region->bytes_per_line_actual);
					
					/* Clamp to the end of the next region. */
					off_t max_pos = (next_region->d_offset + next_region->d_length - 1);
					new_cursor_pos = std::min(max_pos, new_cursor_pos);
				}
			}
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		else if(key == WXK_HOME && (modifiers & wxMOD_CONTROL))
		{
			new_cursor_pos = 0;
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		else if(key == WXK_HOME)
		{
			auto cur_region = _data_region_by_offset(cursor_pos);
			assert(cur_region != NULL);
			
			off_t offset_within_cur  = cursor_pos - cur_region->d_offset;
			off_t offset_within_line = (offset_within_cur % cur_region->bytes_per_line_actual);
			
			new_cursor_pos = cursor_pos - offset_within_line;
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		else if(key == WXK_END && (modifiers & wxMOD_CONTROL))
		{
			new_cursor_pos = doc->buffer_length() - (off_t)(!insert_mode);
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		else if(key == WXK_END)
		{
			auto cur_region = _data_region_by_offset(cursor_pos);
			assert(cur_region != NULL);
			
			off_t offset_within_cur  = cursor_pos - cur_region->d_offset;
			off_t offset_within_line = (offset_within_cur % cur_region->bytes_per_line_actual);
			
			new_cursor_pos = std::min(
				(cursor_pos + ((cur_region->bytes_per_line_actual - offset_within_line) - 1)),
				((cur_region->d_offset + cur_region->d_length) - 1));
			
			if(cursor_state == CSTATE_HEX_MID)
			{
				cursor_state = CSTATE_HEX;
			}
		}
		
		set_cursor_position(new_cursor_pos);
		
		if(modifiers & wxMOD_SHIFT)
		{
			off_t selection_end = selection_off + selection_length;
			
			if(new_cursor_pos < cursor_pos)
			{
				if(selection_length > 0)
				{
					if(selection_off >= cursor_pos)
					{
						assert(selection_end >= new_cursor_pos);
						set_selection(new_cursor_pos, (selection_end - new_cursor_pos));
					}
					else{
						if(new_cursor_pos < selection_off)
						{
							set_selection(new_cursor_pos, (selection_off - new_cursor_pos));
						}
						else{
							set_selection(selection_off, (new_cursor_pos - selection_off));
						}
					}
				}
				else{
					set_selection(new_cursor_pos, (cursor_pos - new_cursor_pos));
				}
			}
			else if(new_cursor_pos > cursor_pos)
			{
				if(selection_length > 0)
				{
					if(selection_off >= cursor_pos)
					{
						if(new_cursor_pos >= selection_end)
						{
							set_selection(selection_end, (new_cursor_pos - selection_end));
						}
						else{
							set_selection(new_cursor_pos, (selection_end - new_cursor_pos));
						}
					}
					else{
						set_selection(selection_off, (new_cursor_pos - selection_off));
					}
				}
				else{
					set_selection(cursor_pos, (new_cursor_pos - cursor_pos));
				}
			}
		}
		else{
			clear_selection();
		}
	}
	
	/* Unhandled key press - propagate to parent. */
	event.Skip();
}

void REHex::DocumentCtrl::OnLeftDown(wxMouseEvent &event)
{
	wxClientDC dc(this);
	
	int mouse_x = event.GetX();
	int rel_x   = mouse_x + this->scroll_xoff;
	int mouse_y = event.GetY();
	
	/* Iterate over the regions to find the last one which does NOT start beyond the current
	 * scroll_y.
	*/
	
	auto region = regions.begin();
	for(auto next = std::next(region); next != regions.end() && (*next)->y_offset <= scroll_yoff; ++next)
	{
		region = next;
	}
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		REHex::DocumentCtrl::DataRegion    *dr = dynamic_cast<REHex::DocumentCtrl::DataRegion*>   (*region);
		REHex::DocumentCtrl::CommentRegion *cr = dynamic_cast<REHex::DocumentCtrl::CommentRegion*>(*region);
		
		if(dr != NULL)
		{
			if(rel_x < offset_column_width)
			{
				/* Click was within the offset area */
			}
			else if(show_ascii && rel_x >= dr->ascii_text_x)
			{
				/* Click was within the ASCII area */
				
				off_t clicked_offset = dr->offset_near_xy_ascii(*this, rel_x, line_off);
				if(clicked_offset >= 0)
				{
					/* Clicked on a character */
					
					if(event.ShiftDown())
					{
						off_t old_position = (mouse_shift_initial >= 0 ? mouse_shift_initial : get_cursor_position());
						_set_cursor_position(clicked_offset, CSTATE_ASCII);
						
						if(clicked_offset > old_position)
						{
							set_selection(old_position, (clicked_offset - old_position));
						}
						else{
							set_selection(clicked_offset, (old_position - clicked_offset));
						}
						
						mouse_shift_initial  = old_position;
						mouse_down_at_offset = clicked_offset;
						mouse_down_at_x      = rel_x;
						mouse_down_in_ascii  = true;
					}
					else{
						_set_cursor_position(clicked_offset, CSTATE_ASCII);
						
						clear_selection();
						
						mouse_down_at_offset = clicked_offset;
						mouse_down_at_x      = rel_x;
						mouse_down_in_ascii  = true;
					}
					
					CaptureMouse();
					mouse_select_timer.Start(MOUSE_SELECT_INTERVAL, wxTIMER_CONTINUOUS);
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
			else{
				/* Click was within the hex area */
				
				off_t clicked_offset = dr->offset_near_xy_hex(*this, rel_x, line_off);
				if(clicked_offset >= 0)
				{
					/* Clicked on a byte */
					
					if(event.ShiftDown())
					{
						off_t old_position = (mouse_shift_initial >= 0 ? mouse_shift_initial : get_cursor_position());
						_set_cursor_position(clicked_offset, CSTATE_HEX);
						
						if(clicked_offset > old_position)
						{
							set_selection(old_position, (clicked_offset - old_position));
						}
						else{
							set_selection(clicked_offset, (old_position - clicked_offset));
						}
						
						mouse_shift_initial  = old_position;
						mouse_down_at_offset = old_position;
						mouse_down_at_x      = rel_x;
						mouse_down_in_hex    = true;
					}
					else{
						_set_cursor_position(clicked_offset, CSTATE_HEX);
						
						clear_selection();
						
						mouse_down_at_offset = clicked_offset;
						mouse_down_at_x      = rel_x;
						mouse_down_in_hex    = true;
					}
					
					CaptureMouse();
					mouse_select_timer.Start(MOUSE_SELECT_INTERVAL, wxTIMER_CONTINUOUS);
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
		}
		else if(cr != NULL)
		{
			/* Mouse was clicked within a Comment region, ensure we are within the border drawn around the
			 * comment text.
			*/
			
			int hf_width = hf_char_width();
			int indent_width = _indent_width(cr->indent_depth);
			
			if(
				(line_off > 0 || (mouse_y % hf_height) >= (hf_height / 4)) /* Not above top edge. */
				&& (line_off < (cr->y_lines - 1) || (mouse_y % hf_height) <= ((hf_height / 4) * 3)) /* Not below bottom edge. */
				&& rel_x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
				&& rel_x < ((virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
			{
				OffsetLengthEvent event(this, COMMENT_LEFT_CLICK, cr->c_offset, cr->c_length);
				ProcessEvent(event);
			}
		}
	}
	
	/* Document takes focus when clicked. */
	SetFocus();
}

void REHex::DocumentCtrl::OnLeftUp(wxMouseEvent &event)
{
	if(mouse_down_in_hex || mouse_down_in_ascii)
	{
		mouse_select_timer.Stop();
		ReleaseMouse();
	}
	
	mouse_down_in_hex   = false;
	mouse_down_in_ascii = false;
}

void REHex::DocumentCtrl::OnRightDown(wxMouseEvent &event)
{
	/* If the user right clicks while selecting, and then releases the left button over the
	 * menu, we never receive the EVT_LEFT_UP event. Release the mouse and cancel the selection
	 * now, else we wind up keeping the mouse grabbed and stop it interacting with any other
	 * windows...
	*/
	
	if(mouse_down_in_hex || mouse_down_in_ascii)
	{
		mouse_select_timer.Stop();
		ReleaseMouse();
		
		mouse_down_in_hex   = false;
		mouse_down_in_ascii = false;
	}
	
	wxClientDC dc(this);
	
	int mouse_x = event.GetX();
	int rel_x   = mouse_x + this->scroll_xoff;
	int mouse_y = event.GetY();
	
	/* Iterate over the regions to find the last one which does NOT start beyond the current
	 * scroll_y.
	*/
	
	auto region = regions.begin();
	for(auto next = std::next(region); next != regions.end() && (*next)->y_offset <= scroll_yoff; ++next)
	{
		region = next;
	}
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		REHex::DocumentCtrl::DataRegion *dr = dynamic_cast<REHex::DocumentCtrl::DataRegion*>(*region);
		if(dr != NULL)
		{
			if(rel_x < offset_column_width)
			{
				/* Click was within the offset area */
			}
			else if(show_ascii && rel_x >= dr->ascii_text_x)
			{
				/* Click was within the ASCII area */
				
				off_t clicked_offset = dr->offset_at_xy_ascii(*this, rel_x, line_off);
				if(clicked_offset >= 0)
				{
					/* Clicked on a character */
					
					_set_cursor_position(clicked_offset, CSTATE_ASCII);
					
					if(clicked_offset < selection_off || clicked_offset >= selection_off + selection_length)
					{
						clear_selection();
					}
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
			else{
				/* Click was within the hex area */
				
				off_t clicked_offset = dr->offset_at_xy_hex(*this, rel_x, line_off);
				if(clicked_offset >= 0)
				{
					/* Clicked on a byte */
					
					_set_cursor_position(clicked_offset, CSTATE_HEX);
					
					if(clicked_offset < selection_off || clicked_offset >= selection_off + selection_length)
					{
						clear_selection();
					}
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
			
			wxCommandEvent event(DATA_RIGHT_CLICK, GetId());
			event.SetEventObject(this);
			
			ProcessEvent(event);
		}
		
		REHex::DocumentCtrl::CommentRegion *cr = dynamic_cast<REHex::DocumentCtrl::CommentRegion*>(*region);
		if(cr != NULL)
		{
			/* Mouse was clicked within a Comment region, ensure we are within the border drawn around the
			 * comment text.
			*/
			
			int hf_width = hf_char_width();
			int indent_width = _indent_width(cr->indent_depth);
			
			if(
				(line_off > 0 || (mouse_y % hf_height) >= (hf_height / 4)) /* Not above top edge. */
				&& (line_off < (cr->y_lines - 1) || (mouse_y % hf_height) <= ((hf_height / 4) * 3)) /* Not below bottom edge. */
				&& rel_x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
				&& rel_x < ((virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
			{
				OffsetLengthEvent event(this, COMMENT_RIGHT_CLICK, cr->c_offset, cr->c_length);
				ProcessEvent(event);
			}
		}
	}
	
	/* Document takes focus when clicked. */
	SetFocus();
}

void REHex::DocumentCtrl::OnMotion(wxMouseEvent &event)
{
	int mouse_x = event.GetX();
	int mouse_y = event.GetY();
	
	int rel_x = mouse_x + scroll_xoff;
	
	/* Iterate over the regions to find the last one which does NOT start beyond the current
	 * scroll_y.
	*/
	
	auto region = regions.begin();
	for(auto next = std::next(region); next != regions.end() && (*next)->y_offset <= scroll_yoff; ++next)
	{
		region = next;
	}
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	wxCursor cursor = wxNullCursor;
	
	if(region != regions.end())
	{
		cursor = (*region)->cursor_for_point(*this, rel_x, line_off, (mouse_y % hf_height));
	}
	
	SetCursor(cursor);
	
	OnMotionTick(event.GetX(), event.GetY());
}

void REHex::DocumentCtrl::OnSelectTick(wxTimerEvent &event)
{
	wxPoint window_pos = GetScreenPosition();
	wxPoint mouse_pos  = wxGetMousePosition();
	
	OnMotionTick((mouse_pos.x - window_pos.x), (mouse_pos.y - window_pos.y));
}

void REHex::DocumentCtrl::OnMotionTick(int mouse_x, int mouse_y)
{
	if(!mouse_down_in_ascii && !mouse_down_in_hex)
	{
		return;
	}
	
	wxClientDC dc(this);
	
	int scroll_xoff_max = GetScrollRange(wxHORIZONTAL) - GetScrollThumb(wxHORIZONTAL);
	
	if(mouse_x < 0)
	{
		scroll_xoff -= std::min(abs(mouse_x), scroll_xoff);
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		
		mouse_x = 0;
	}
	else if(mouse_x >= client_width)
	{
		scroll_xoff += std::min((int)(mouse_x - client_width), (scroll_xoff_max - scroll_xoff));
		SetScrollPos(wxHORIZONTAL, scroll_xoff);
		
		mouse_x = client_width - 1;
	}
	
	if(mouse_y < 0)
	{
		scroll_yoff -= std::min((int64_t)(abs(mouse_y) / hf_height + 1), scroll_yoff);
		_update_vscroll_pos();
		
		mouse_y = 0;
	}
	else if(mouse_y >= client_height)
	{
		scroll_yoff += std::min((int64_t)((mouse_y - client_height) / hf_height + 1), (scroll_yoff_max - scroll_yoff));
		_update_vscroll_pos();
		
		mouse_y = client_height - 1;
	}
	
	int rel_x = mouse_x + scroll_xoff;
	
	/* Iterate over the regions to find the last one which does NOT start beyond the current
	 * scroll_y.
	*/
	
	auto region = regions.begin();
	for(auto next = std::next(region); next != regions.end() && (*next)->y_offset <= scroll_yoff; ++next)
	{
		region = next;
	}
	
	/* If we are scrolled past the start of the regiomn, will need to skip some of the first one. */
	int64_t skip_lines_in_region = (this->scroll_yoff - (*region)->y_offset);
	
	int64_t line_off = (mouse_y / hf_height) + skip_lines_in_region;
	
	while(region != regions.end() && line_off >= (*region)->y_lines)
	{
		line_off -= (*region)->y_lines;
		++region;
	}
	
	if(region != regions.end())
	{
		REHex::DocumentCtrl::DataRegion *dr = dynamic_cast<REHex::DocumentCtrl::DataRegion*>(*region);
		REHex::DocumentCtrl::CommentRegion *cr;
		if(dr != NULL)
		{
			if(mouse_down_in_hex)
			{
				/* Started dragging in hex area */
				
				off_t select_to_offset = dr->offset_near_xy_hex(*this, rel_x, line_off);
				if(select_to_offset >= 0)
				{
					off_t new_sel_off, new_sel_len;
					
					if(select_to_offset >= mouse_down_at_offset)
					{
						new_sel_off = mouse_down_at_offset;
						new_sel_len = (select_to_offset - mouse_down_at_offset) + 1;
					}
					else{
						new_sel_off = select_to_offset;
						new_sel_len = (mouse_down_at_offset - select_to_offset) + 1;
					}
					
					if(new_sel_len == 1 && abs(rel_x - mouse_down_at_x) < hf_char_width())
					{
						clear_selection();
					}
					else{
						set_selection(new_sel_off, new_sel_len);
					}
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
			else if(mouse_down_in_ascii)
			{
				/* Started dragging in ASCII area */
				
				off_t select_to_offset = dr->offset_near_xy_ascii(*this, rel_x, line_off);
				if(select_to_offset >= 0)
				{
					off_t new_sel_off, new_sel_len;
					
					if(select_to_offset >= mouse_down_at_offset)
					{
						new_sel_off = mouse_down_at_offset;
						new_sel_len = (select_to_offset - mouse_down_at_offset) + 1;
					}
					else{
						new_sel_off = select_to_offset;
						new_sel_len = (mouse_down_at_offset - select_to_offset) + 1;
					}
					
					if(new_sel_len == 1 && abs(rel_x - mouse_down_at_x) < (hf_char_width() / 2))
					{
						clear_selection();
					}
					else{
						set_selection(new_sel_off, new_sel_len);
					}
					
					/* TODO: Limit paint to affected area */
					Refresh();
				}
			}
		}
		else if((cr = dynamic_cast<REHex::DocumentCtrl::CommentRegion*>(*region)) != NULL)
		{
			if(mouse_down_in_hex || mouse_down_in_ascii)
			{
				off_t select_to_offset = cr->c_offset;
				off_t new_sel_off, new_sel_len;
				
				if(select_to_offset >= mouse_down_at_offset)
				{
					new_sel_off = mouse_down_at_offset;
					new_sel_len = select_to_offset - mouse_down_at_offset;
				}
				else{
					new_sel_off = select_to_offset;
					new_sel_len = (mouse_down_at_offset - select_to_offset) + 1;
				}
				
				if(new_sel_len == 1 && abs(rel_x - mouse_down_at_x) < (hf_char_width() / 2))
				{
					clear_selection();
				}
				else{
					set_selection(new_sel_off, new_sel_len);
				}
				
				/* TODO: Limit paint to affected area */
				Refresh();
			}
		}
	}
}

void REHex::DocumentCtrl::OnRedrawCursor(wxTimerEvent &event)
{
	cursor_visible = !cursor_visible;
	
	/* TODO: Limit paint to cursor area */
	Refresh();
}

void REHex::DocumentCtrl::_recalc_regions(wxDC &dc)
{
	uint64_t next_yo = 0;
	
	for(auto i = regions.begin(); i != regions.end(); ++i)
	{
		(*i)->y_offset = next_yo;
		(*i)->update_lines(*this, dc);
		
		next_yo += (*i)->y_lines;
	}
}

REHex::DocumentCtrl::DataRegion *REHex::DocumentCtrl::_data_region_by_offset(off_t offset)
{
	for(auto region = regions.begin(); region != regions.end(); ++region)
	{
		auto dr = dynamic_cast<REHex::DocumentCtrl::DataRegion*>(*region);
		if(dr != NULL
			&& dr->d_offset <= offset
			&& ((dr->d_offset + dr->d_length) > offset
				|| ((dr->d_offset + dr->d_length) == offset && doc->buffer_length() == offset)))
		{
			return dr;
		}
	}
	
	return NULL;
}

/* Scroll the Document vertically to make the given line visible.
 * Does nothing if the line is already on-screen.
*/
void REHex::DocumentCtrl::_make_line_visible(int64_t line)
{
	if(scroll_yoff > line)
	{
		/* Need to scroll up, line will be at the top. */
		scroll_yoff = line;
	}
	else if((scroll_yoff + visible_lines) <= line)
	{
		/* Need to scroll down, line will be the last fully-visible one. */
		scroll_yoff = (line - visible_lines) + !!visible_lines;
	}
	else{
		/* Don't need to scroll. */
		return;
	}
	
	assert(scroll_yoff <= line);
	assert((scroll_yoff + visible_lines + !visible_lines) > line);
	
	_update_vscroll_pos();
	Refresh();
}

/* Scroll the Document horizontally to (try to) make the given range of X co-ordinates visible.
 * Does nothing if the range is fully visible.
*/
void REHex::DocumentCtrl::_make_x_visible(int x_px, int width_px)
{
	if(scroll_xoff > x_px)
	{
		/* Scroll to the left */
		scroll_xoff = x_px;
	}
	else if((scroll_xoff + client_width) < (x_px + width_px) && width_px <= client_width)
	{
		/* Scroll to the right. */
		scroll_xoff = x_px - (client_width - width_px);
	}
	else{
		/* Don't need to scroll. */
		return;
	}
	
	assert(scroll_xoff <= x_px);
	assert((scroll_xoff + client_width) >= (x_px + width_px) || width_px > client_width);
	
	SetScrollPos(wxHORIZONTAL, scroll_xoff);
	Refresh();
}

/* Scroll the Document to make the byte at the given offset visible.
 * Does nothing if the byte is already on-screen.
*/
void REHex::DocumentCtrl::_make_byte_visible(off_t offset)
{
	auto dr = _data_region_by_offset(offset);
	assert(dr != NULL);
	
	/* TODO: Move these maths into Region::Data */
	
	off_t region_offset = offset - dr->d_offset;
	
	uint64_t region_line = dr->y_offset + (region_offset / dr->bytes_per_line_actual);
	_make_line_visible(region_line);
	
	off_t line_off = region_offset % dr->bytes_per_line_actual;
	
	if(cursor_state == CSTATE_HEX || cursor_state == CSTATE_HEX_MID)
	{
		unsigned int line_x = offset_column_width
			+ hf_string_width(line_off * 2)
			+ hf_string_width(line_off / bytes_per_group);
		_make_x_visible(line_x, hf_string_width(2));
	}
	else if(cursor_state == CSTATE_ASCII)
	{
		off_t byte_x = dr->ascii_text_x + hf_string_width(line_off);
		_make_x_visible(byte_x, hf_char_width());
	}
}

std::list<wxString> REHex::DocumentCtrl::_format_text(const wxString &text, unsigned int cols, unsigned int from_line, unsigned int max_lines)
{
	assert(cols > 0);
	
	/* TODO: Throw myself into the abyss and support Unicode properly...
	 * (This function assumes one byte is one full-width character on the screen.
	*/
	
	std::list<wxString> lines;
	
	for(size_t at = 0; at < text.size();)
	{
		size_t newline_at = text.find_first_of('\n', at);
		
		if(newline_at != std::string::npos && newline_at <= (at + cols))
		{
			/* There is a newline within one row's worth of text of our current position.
			 * Add all the text up to it and continue from after it.
			*/
			lines.push_back(text.substr(at, newline_at - at));
			at = newline_at + 1;
		}
		else{
			/* The line is too long, just wrap it at whatever character is on the boundary.
			 *
			 * std::string::substr() will clamp the length if it goes beyond the end of
			 * the string.
			*/
			lines.push_back(text.substr(at, cols));
			at += cols;
		}
	}
	
	lines.erase(lines.begin(), std::next(lines.begin(), std::min((size_t)(from_line), lines.size())));
	lines.erase(std::next(lines.begin(), std::min((size_t)(max_lines), lines.size())), lines.end());
	
	return lines;
}

int REHex::DocumentCtrl::_indent_width(int depth)
{
	return hf_char_width() * depth;
}

/* Calculate the width of a character in hex_font. */
int REHex::DocumentCtrl::hf_char_width()
{
	return hf_string_width(1);
}

/* Calculate the bounding box for a string which is length characters long when
 * rendered using hex_font. The string should fit within the box.
 *
 * We can't just multiply the width of a single character because certain
 * platforms *cough* *OSX* use subpixel co-ordinates for character spacing.
*/
int REHex::DocumentCtrl::hf_string_width(int length)
{
	if(length == 0)
	{
		return 0;
	}
	
	if(length <= PRECOMP_HF_STRING_WIDTH_TO)
	{
		return hf_string_width_precomp[length - 1];
	}
	
	wxClientDC dc(this);
	dc.SetFont(*hex_font);
	
	wxSize te = dc.GetTextExtent(std::string(length, 'X'));
	return te.GetWidth();
}

/* Calculate the character at the pixel offset relative to the start of the string. */
int REHex::DocumentCtrl::hf_char_at_x(int x_px)
{
	for(int i = 0;; ++i)
	{
		int w = hf_string_width(i + 1);
		if(w > x_px)
		{
			return i;
		}
	}
}

const std::list<REHex::DocumentCtrl::Region*> &REHex::DocumentCtrl::get_regions() const
{
	return regions;
}

void REHex::DocumentCtrl::append_region(Region *region)
{
	region->y_offset = regions.empty()
		? 0
		: regions.back()->y_offset + regions.back()->y_lines;
	
	wxClientDC dc(this);
	region->update_lines(*this, dc);
	
	regions.push_back(region);
	
	Refresh();
}

void REHex::DocumentCtrl::insert_region(Region *region, std::list<Region*>::const_iterator before_this)
{
	if(before_this == regions.end())
	{
		append_region(region);
		return;
	}
	
	region->y_offset = (*before_this)->y_offset;
	
	wxClientDC dc(this);
	region->update_lines(*this, dc);
	
	regions.insert(before_this, region);
	
	/* std::list::erase() with zero-length range converts const_iterator to iterator via our
	 * non-const reference to the list. Allows us to update elements following the new one
	 * without having to walk the list before it.
	*/
	auto next = regions.erase(before_this, before_this);
	
	for(auto i = next; i != regions.end(); ++i)
	{
		(*i)->y_offset += region->y_lines;
	}
	
	Refresh();
}

void REHex::DocumentCtrl::erase_region(std::list<Region*>::const_iterator i)
{
	int64_t erased_y_lines = (*i)->y_lines;
	
	delete *i;
	auto next = regions.erase(i);
	
	for(auto j = next; j != regions.end(); ++j)
	{
		(*j)->y_offset -= erased_y_lines;
	}
	
	Refresh();
}

void REHex::DocumentCtrl::replace_region(Region *region, std::list<Region*>::const_iterator replace_this)
{
	insert_region(region, replace_this);
	erase_region(replace_this);
}

REHex::DocumentCtrl::Region::Region():
	indent_depth(0), indent_final(0) {}

REHex::DocumentCtrl::Region::~Region() {}

wxCursor REHex::DocumentCtrl::Region::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	return wxNullCursor;
}

void REHex::DocumentCtrl::Region::draw_container(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	if(indent_depth > 0)
	{
		int cw = doc.hf_char_width();
		int ch = doc.hf_height;
		
		int64_t skip_lines = (y < 0 ? (-y / ch) : 0);
		
		int     box_y  = y + (skip_lines * (int64_t)(ch));
		int64_t box_h  = (y_lines - skip_lines) * (int64_t)(ch);
		int     box_hc = std::min(box_h, (int64_t)(doc.client_height));
		
		int box_x = x + (cw / 4);
		int box_w = doc.virtual_width - (cw / 2);
		
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush((*active_palette)[Palette::PAL_NORMAL_TEXT_BG]));
		
		dc.DrawRectangle(0, box_y, doc.client_width, box_hc);
		
		dc.SetPen(wxPen((*active_palette)[Palette::PAL_NORMAL_TEXT_FG]));
		
		for(int i = 0; i < indent_depth; ++i)
		{
			if(box_h < (int64_t)(doc.client_height) && (i + indent_final) == indent_depth)
			{
				box_h  -= ch / 2;
				box_hc -= ch / 2;
			}
			
			dc.DrawLine(box_x, box_y, box_x, (box_y + box_hc));
			dc.DrawLine((box_x + box_w - 1), box_y, (box_x + box_w - 1), (box_y + box_hc));
			
			if(box_h < (int64_t)(doc.client_height) && (i + indent_final) >= indent_depth)
			{
				dc.DrawLine(box_x, (box_y + box_h), (box_x + box_w - 1), (box_y + box_h));
				
				box_h  -= ch;
				box_hc -= ch;
			}
			
			box_x += cw;
			box_w -= cw * 2;
		}
	}
}

REHex::DocumentCtrl::DataRegion::DataRegion(off_t d_offset, off_t d_length, int indent_depth):
	d_offset(d_offset), d_length(d_length), bytes_per_line_actual(1)
{
	assert(d_offset >= 0);
	assert(d_length >= 0);
	
	this->indent_depth = indent_depth;
}

void REHex::DocumentCtrl::DataRegion::update_lines(REHex::DocumentCtrl &doc, wxDC &dc)
{
	int indent_width = doc._indent_width(indent_depth);
	
	offset_text_x = indent_width;
	hex_text_x    = indent_width + doc.offset_column_width;
	ascii_text_x  = (doc.virtual_width - indent_width) - doc.hf_string_width(bytes_per_line_actual);
	
	/* Height of the region is simply the number of complete lines of data plus an incomplete
	 * one if the data isn't a round number of lines.
	*/
	y_lines = (d_length / bytes_per_line_actual) + !!(d_length % bytes_per_line_actual) + indent_final;
	
	if((d_offset + d_length) == doc.doc->buffer_length() && (d_length % bytes_per_line_actual) == 0)
	{
		/* This is the last data region in the document. Make it one row taller if the last
		 * row is full so there is always somewhere to draw the insert cursor.
		*/
		++y_lines;
	}
}

void REHex::DocumentCtrl::DataRegion::draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	draw_container(doc, dc, x, y);
	
	dc.SetFont(*(doc.hex_font));
	
	wxPen norm_fg_1px((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1);
	wxPen selected_bg_1px((*active_palette)[Palette::PAL_SELECTED_TEXT_BG], 1);
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	
	bool alternate_row = true;
	
	auto normal_text_colour = [&dc,&alternate_row]()
	{
		dc.SetTextForeground((*active_palette)[alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG ]);
		dc.SetBackgroundMode(wxTRANSPARENT);
	};
	
	/* If we are scrolled part-way into a data region, don't render data above the client area
	 * as it would get expensive very quickly with large files.
	*/
	int64_t skip_lines = (y < 0 ? (-y / doc.hf_height) : 0);
	off_t skip_bytes  = skip_lines * bytes_per_line_actual;
	
	if(skip_lines >= (y_lines - indent_final))
	{
		/* All of our data is past the top of the client area, all that needed to be
		 * rendered is the bottom of the container around it.
		*/
		return;
	}
	
	/* Increment y up to our real drawing start point. We can now trust it to be within a
	 * hf_height of zero, not the stratospheric integer-overflow-causing values it could
	 * previously have on huge files.
	*/
	y += skip_lines * doc.hf_height;
	
	/* The maximum amount of data that can be drawn on the screen before we're past the bottom
	 * of the client area. Drawing more than this would be pointless and very expensive in the
	 * case of large files.
	*/
	int max_lines = ((doc.client_height - y) / doc.hf_height) + 1;
	off_t max_bytes = (off_t)(max_lines) * (off_t)(bytes_per_line_actual);
	
	if((int64_t)(max_lines) > (y_lines - indent_final - skip_lines))
	{
		max_lines = (y_lines - indent_final - skip_lines);
	}
	
	if(doc.offset_column)
	{
		int offset_vl_x = (x + offset_text_x + doc.offset_column_width) - (doc.hf_char_width() / 2);
		
		dc.SetPen(norm_fg_1px);
		dc.DrawLine(offset_vl_x, y, offset_vl_x, y + (max_lines * doc.hf_height));
	}
	
	if(doc.show_ascii)
	{
		int ascii_vl_x = (x + ascii_text_x) - (doc.hf_char_width() / 2);
		
		dc.SetPen(norm_fg_1px);
		dc.DrawLine(ascii_vl_x, y, ascii_vl_x, y + (max_lines * doc.hf_height));
	}
	
	/* Fetch the data to be drawn. */
	std::vector<unsigned char> data;
	bool data_err = false;
	
	try {
		data = doc.doc->read_data(d_offset + skip_bytes, std::min(max_bytes, (d_length - std::min(skip_bytes, d_length))));
	}
	catch(const std::exception &e)
	{
		fprintf(stderr, "Exception in REHex::DocumentCtrl::DataRegion::draw: %s\n", e.what());
		
		data.insert(data.end(), std::min(max_bytes, (d_length - std::min(skip_bytes, d_length))), '?');
		data_err = true;
	}
	
	/* The offset of the character in the Buffer currently being drawn. */
	off_t cur_off = d_offset + skip_bytes;
	
	bool hex_active   = doc.HasFocus() && doc.cursor_state != CSTATE_ASCII;
	bool ascii_active = doc.HasFocus() && doc.cursor_state == CSTATE_ASCII;
	
	off_t cursor_pos = doc.get_cursor_position();
	
	size_t secondary_selection_remain = 0;
	
	for(auto di = data.begin();;)
	{
		alternate_row = !alternate_row;
		
		if(doc.offset_column)
		{
			/* Draw the offsets to the left */
			
			std::string offset_str = format_offset(cur_off, doc.offset_display_base, doc.doc->buffer_length());
			
			normal_text_colour();
			dc.DrawText(offset_str.c_str(), (x + offset_text_x), y);
		}
		
		int hex_base_x = x + hex_text_x;  /* Base X co-ordinate to draw hex characters from */
		int hex_x      = hex_base_x;      /* X co-ordinate of current hex character */
		int hex_x_char = 0;               /* Column of current hex character */
		
		int ascii_base_x = x + ascii_text_x;  /* Base X co-ordinate to draw ASCII characters from */
		int ascii_x      = ascii_base_x;      /* X co-ordinate of current ASCII character */
		int ascii_x_char = 0;                 /* Column of current ASCII character */
		
		auto draw_end_cursor = [&]()
		{
			if((doc.cursor_visible && doc.cursor_state == CSTATE_HEX) || !hex_active)
			{
				if(doc.insert_mode || !hex_active)
				{
					dc.SetPen(norm_fg_1px);
					dc.DrawLine(hex_x, y, hex_x, y + doc.hf_height);
				}
				else{
					/* Draw the cursor in red if trying to overwrite at an invalid
					 * position. Should only happen in empty files.
					*/
					dc.SetPen(*wxRED_PEN);
					dc.DrawLine(hex_x, y, hex_x, y + doc.hf_height);
				}
			}
			
			if(doc.show_ascii && ((doc.cursor_visible && doc.cursor_state == CSTATE_ASCII) || !ascii_active))
			{
				if(doc.insert_mode || !ascii_active)
				{
					dc.SetPen(norm_fg_1px);
					dc.DrawLine(ascii_x, y, ascii_x, y + doc.hf_height);
				}
				else{
					/* Draw the cursor in red if trying to overwrite at an invalid
					 * position. Should only happen in empty files.
					*/
					dc.SetPen(*wxRED_PEN);
					dc.DrawLine(ascii_x, y, ascii_x, y + doc.hf_height);
				}
			}
		};
		
		if(di == data.end())
		{
			if(cur_off == cursor_pos)
			{
				draw_end_cursor();
			}
			
			break;
		}
		
		/* Calling wxDC::DrawText() for each individual character on the screen is
		 * painfully slow, so we batch up the wxDC::DrawText() calls for each colour and
		 * area on a per-line basis.
		 *
		 * The key of the deferred_drawtext map is the X co-ordinate to render the string
		 * at (hex_base_x or ascii_base_x) and the foreground colour to use.
		 *
		 * The draw_char_deferred() function adds a character to be drawn to the map, while
		 * prefixing it with any spaces necessary to pad it to the correct column from the
		 * base X co-ordinate.
		*/
		
		std::map<std::pair<int, Palette::ColourIndex>, std::string> deferred_drawtext;
		
		auto draw_char_deferred = [&](int base_x, Palette::ColourIndex colour_idx, int col, char ch)
		{
			std::pair<int, Palette::ColourIndex> k(base_x, colour_idx);
			std::string &str = deferred_drawtext[k];
			
			assert(str.length() <= col);
			
			str.append((col - str.length()), ' ');
			str.append(1, ch);
		};
		
		/* Because we need to minimise wxDC::DrawText() calls (see above), we draw any
		 * background colours ourselves and set the background mode to transparent when
		 * drawing text, which enables us to skip over characters that shouldn't be
		 * touched by that particular wxDC::DrawText() call by inserting spaces.
		*/
		
		auto fill_char_bg = [&](int char_x, Palette::ColourIndex colour_idx)
		{
			wxBrush bg_brush((*active_palette)[colour_idx]);
			
			dc.SetBrush(bg_brush);
			dc.SetPen(*wxTRANSPARENT_PEN);
			
			dc.DrawRectangle(char_x, y, doc.hf_char_width(), doc.hf_height);
		};
		
		for(unsigned int c = 0; c < bytes_per_line_actual && di != data.end(); ++c)
		{
			if(c > 0 && (c % doc.bytes_per_group) == 0)
			{
				hex_x = hex_base_x + doc.hf_string_width(++hex_x_char);
			}
			
			unsigned char byte        = *(di++);
			unsigned char high_nibble = (byte & 0xF0) >> 4;
			unsigned char low_nibble  = (byte & 0x0F);
			
			auto highlight = highlight_at_off(cur_off);
			
			auto draw_nibble = [&](unsigned char nibble, bool invert)
			{
				const char *nibble_to_hex = data_err
					? "????????????????"
					: "0123456789ABCDEF";
				
				if(invert && doc.cursor_visible)
				{
					fill_char_bg(hex_x, Palette::PAL_INVERT_TEXT_BG);
					draw_char_deferred(hex_base_x, Palette::PAL_INVERT_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
				}
				else if(cur_off >= doc.selection_off
					&& cur_off < (doc.selection_off + doc.selection_length)
					&& hex_active)
				{
					fill_char_bg(hex_x, Palette::PAL_SELECTED_TEXT_BG);
					draw_char_deferred(hex_base_x, Palette::PAL_SELECTED_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
				}
				else if(secondary_selection_remain > 0 && !(cur_off >= doc.selection_off && cur_off < (doc.selection_off + doc.selection_length)))
				{
					fill_char_bg(hex_x, Palette::PAL_SECONDARY_SELECTED_TEXT_BG);
					draw_char_deferred(hex_base_x, Palette::PAL_SECONDARY_SELECTED_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
				}
				else if(highlight.enable)
				{
					fill_char_bg(hex_x, highlight.bg_colour_idx);
					draw_char_deferred(hex_base_x, highlight.fg_colour_idx, hex_x_char, nibble_to_hex[nibble]);
				}
				else{
					draw_char_deferred(hex_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, hex_x_char, nibble_to_hex[nibble]);
				}
				
				hex_x = hex_base_x + doc.hf_string_width(++hex_x_char);
			};
			
			bool inv_high, inv_low;
			if(cur_off == cursor_pos && hex_active)
			{
				if(doc.cursor_state == CSTATE_HEX)
				{
					inv_high = !doc.insert_mode;
					inv_low  = !doc.insert_mode;
				}
				else /* if(doc.cursor_state == CSTATE_HEX_MID) */
				{
					inv_high = false;
					inv_low  = true;
				}
			}
			else{
				inv_high = false;
				inv_low  = false;
			}
			
			/* Need the current hex_x value for drawing any boxes or insert cursors
			 * below, before it gets updated by draw_nibble().
			*/
			const int pd_hx = hex_x;
			
			draw_nibble(high_nibble, inv_high);
			draw_nibble(low_nibble,  inv_low);
			
			if(cur_off >= doc.selection_off && cur_off < (doc.selection_off + doc.selection_length) && !hex_active)
			{
				dc.SetPen(selected_bg_1px);
				
				if(cur_off == doc.selection_off || c == 0)
				{
					/* Draw vertical line left of selection. */
					dc.DrawLine(pd_hx, y, pd_hx, (y + doc.hf_height));
				}
				
				if(cur_off == (doc.selection_off + doc.selection_length - 1) || c == (bytes_per_line_actual - 1))
				{
					/* Draw vertical line right of selection. */
					dc.DrawLine((pd_hx + doc.hf_string_width(2) - 1), y, (pd_hx + doc.hf_string_width(2) - 1), (y + doc.hf_height));
				}
				
				if(cur_off < (doc.selection_off + bytes_per_line_actual))
				{
					/* Draw horizontal line above selection. */
					dc.DrawLine(pd_hx, y, (pd_hx + doc.hf_string_width(2)), y);
				}
				
				if(cur_off > doc.selection_off && cur_off <= (doc.selection_off + bytes_per_line_actual) && c > 0 && (c % doc.bytes_per_group) == 0)
				{
					/* Draw horizontal line above gap along top of selection. */
					dc.DrawLine((pd_hx - doc.hf_char_width()), y, pd_hx, y);
				}
				
				if(cur_off >= (doc.selection_off + doc.selection_length - bytes_per_line_actual))
				{
					/* Draw horizontal line below selection. */
					dc.DrawLine(pd_hx, (y + doc.hf_height - 1), (pd_hx + doc.hf_string_width(2)), (y + doc.hf_height - 1));
					
					if(c > 0 && (c % doc.bytes_per_group) == 0 && cur_off > doc.selection_off)
					{
						/* Draw horizontal line below gap along bottom of selection. */
						dc.DrawLine((pd_hx - doc.hf_char_width()), (y + doc.hf_height - 1), pd_hx, (y + doc.hf_height - 1));
					}
				}
			}
			
			if(cur_off == cursor_pos && doc.insert_mode && ((doc.cursor_visible && doc.cursor_state == CSTATE_HEX) || !hex_active))
			{
				/* Draw insert cursor. */
				dc.SetPen(norm_fg_1px);
				dc.DrawLine(pd_hx, y, pd_hx, y + doc.hf_height);
			}
			
			if(cur_off == cursor_pos && !doc.insert_mode && !hex_active)
			{
				/* Draw inactive overwrite cursor. */
				dc.SetBrush(*wxTRANSPARENT_BRUSH);
				dc.SetPen(norm_fg_1px);
				
				if(doc.cursor_state == CSTATE_HEX_MID)
				{
					dc.DrawRectangle(pd_hx + doc.hf_char_width(), y, doc.hf_char_width(), doc.hf_height);
				}
				else{
					dc.DrawRectangle(pd_hx, y, doc.hf_string_width(2), doc.hf_height);
				}
			}
			
			if(doc.show_ascii)
			{
				char ascii_byte = isasciiprint(byte)
					? byte
					: '.';
				
				if(ascii_active)
				{
					if(cur_off == cursor_pos && !doc.insert_mode && doc.cursor_visible)
					{
						fill_char_bg(ascii_x, Palette::PAL_INVERT_TEXT_BG);
						draw_char_deferred(ascii_base_x, Palette::PAL_INVERT_TEXT_FG, ascii_x_char, ascii_byte);
					}
					else if(cur_off >= doc.selection_off && cur_off < (doc.selection_off + doc.selection_length))
					{
						fill_char_bg(ascii_x, Palette::PAL_SELECTED_TEXT_BG);
						draw_char_deferred(ascii_base_x, Palette::PAL_SELECTED_TEXT_FG, ascii_x_char, ascii_byte);
					}
					else if(secondary_selection_remain > 0)
					{
						fill_char_bg(ascii_x, Palette::PAL_SECONDARY_SELECTED_TEXT_BG);
						draw_char_deferred(ascii_base_x, Palette::PAL_SECONDARY_SELECTED_TEXT_FG, ascii_x_char, ascii_byte);
					}
					else if(highlight.enable)
					{
						fill_char_bg(ascii_x, highlight.bg_colour_idx);
						draw_char_deferred(ascii_base_x, highlight.fg_colour_idx, ascii_x_char, ascii_byte);
					}
					else{
						draw_char_deferred(ascii_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, ascii_x_char, ascii_byte);
					}
				}
				else{
					if(secondary_selection_remain > 0 && !(cur_off >= doc.selection_off && cur_off < (doc.selection_off + doc.selection_length)) && !ascii_active)
					{
						fill_char_bg(ascii_x, Palette::PAL_SECONDARY_SELECTED_TEXT_BG);
						draw_char_deferred(ascii_base_x, Palette::PAL_SECONDARY_SELECTED_TEXT_FG, ascii_x_char, ascii_byte);
					}
					else if(highlight.enable && !ascii_active)
					{
						fill_char_bg(ascii_x, highlight.bg_colour_idx);
						draw_char_deferred(ascii_base_x, highlight.fg_colour_idx, ascii_x_char, ascii_byte);
					}
					else{
						draw_char_deferred(ascii_base_x, alternate_row ? Palette::PAL_ALTERNATE_TEXT_FG : Palette::PAL_NORMAL_TEXT_FG, ascii_x_char, ascii_byte);
					}
					
					if(cur_off == cursor_pos && !doc.insert_mode)
					{
						dc.SetBrush(*wxTRANSPARENT_BRUSH);
						dc.SetPen(norm_fg_1px);
						
						dc.DrawRectangle(ascii_x, y, doc.hf_char_width(), doc.hf_height);
					}
					else if(cur_off >= doc.selection_off && cur_off < (doc.selection_off + doc.selection_length))
					{
						dc.SetPen(selected_bg_1px);
						
						if(cur_off == doc.selection_off || c == 0)
						{
							/* Draw vertical line left of selection. */
							dc.DrawLine(ascii_x, y, ascii_x, (y + doc.hf_height));
						}
						
						if(cur_off == (doc.selection_off + doc.selection_length - 1) || c == (bytes_per_line_actual - 1))
						{
							/* Draw vertical line right of selection. */
							dc.DrawLine((ascii_x + doc.hf_char_width() - 1), y, (ascii_x + doc.hf_char_width() - 1), (y + doc.hf_height));
						}
						
						if(cur_off < (doc.selection_off + bytes_per_line_actual))
						{
							/* Draw horizontal line above selection. */
							dc.DrawLine(ascii_x, y, (ascii_x + doc.hf_char_width()), y);
						}
						
						if(cur_off >= (doc.selection_off + doc.selection_length - bytes_per_line_actual))
						{
							/* Draw horizontal line below selection. */
							dc.DrawLine(ascii_x, (y + doc.hf_height - 1), (ascii_x + doc.hf_char_width()), (y + doc.hf_height - 1));
						}
					}
				}
				
				if(cur_off == cursor_pos && doc.insert_mode && (doc.cursor_visible || !ascii_active))
				{
					dc.SetPen(norm_fg_1px);
					dc.DrawLine(ascii_x, y, ascii_x, y + doc.hf_height);
				}
				
				ascii_x = ascii_base_x + doc.hf_string_width(++ascii_x_char);
			}
			
			++cur_off;
			
			if(secondary_selection_remain > 0)
			{
				--secondary_selection_remain;
			}
		}
		
		normal_text_colour();
		
		for(auto dd = deferred_drawtext.begin(); dd != deferred_drawtext.end(); ++dd)
		{
			dc.SetTextForeground((*active_palette)[dd->first.second]);
			dc.SetBackgroundMode(wxTRANSPARENT);
			
			dc.DrawText(dd->second, dd->first.first, y);
		}
		
		if(cur_off == cursor_pos && cur_off == doc.doc->buffer_length() && (d_length % bytes_per_line_actual) != 0)
		{
			draw_end_cursor();
		}
		
		y += doc.hf_height;
		
		if(di == data.end() && (cur_off < doc.doc->buffer_length() || (d_length % bytes_per_line_actual) != 0))
		{
			break;
		}
	}
}

wxCursor REHex::DocumentCtrl::DataRegion::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	if(x >= hex_text_x)
	{
		return wxCursor(wxCURSOR_IBEAM);
	}
	else{
		return wxNullCursor;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_at_xy_hex(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	if(mouse_x_px < hex_text_x)
	{
		return -1;
	}
	
	mouse_x_px -= hex_text_x;
	
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = d_offset + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	if(((char_offset + 1) % ((doc.bytes_per_group * 2) + 1)) == 0)
	{
		/* Click was over a space between byte groups. */
		return -1;
	}
	else{
		unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((doc.bytes_per_group * 2) + 1));
		unsigned int line_offset_bytes      = char_offset_sub_spaces / 2;
		off_t clicked_offset                = line_data_begin + line_offset_bytes;
		
		if(clicked_offset < line_data_end)
		{
			/* Clicked on a byte */
			return clicked_offset;
		}
		else{
			/* Clicked past the end of the line */
			return -1;
		}
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_at_xy_ascii(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	if(!doc.show_ascii || mouse_x_px < ascii_text_x)
	{
		return -1;
	}
	
	mouse_x_px -= ascii_text_x;
	
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = d_offset + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	off_t clicked_offset     = line_data_begin + char_offset;
	
	if(clicked_offset < line_data_end)
	{
		/* Clicked on a character */
		return clicked_offset;
	}
	else{
		/* Clicked past the end of the line */
		return -1;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_near_xy_hex(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = d_offset + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	if(mouse_x_px < hex_text_x)
	{
		/* Mouse is in offset area, return offset of last byte of previous line. */
		return line_data_begin - 1;
	}
	
	mouse_x_px -= hex_text_x;
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	
	unsigned int char_offset_sub_spaces = char_offset - (char_offset / ((doc.bytes_per_group * 2) + 1));
	unsigned int line_offset_bytes      = char_offset_sub_spaces / 2;
	off_t clicked_offset                = line_data_begin + line_offset_bytes;
	
	if(clicked_offset < line_data_end)
	{
		/* Mouse is on a byte. */
		return clicked_offset;
	}
	else{
		/* Mouse is past end of line, return last byte of this line. */
		return line_data_end - 1;
	}
}

off_t REHex::DocumentCtrl::DataRegion::offset_near_xy_ascii(REHex::DocumentCtrl &doc, int mouse_x_px, uint64_t mouse_y_lines)
{
	/* Calculate the offset within the Buffer of the first byte on this line
	 * and the offset (plus one) of the last byte on this line.
	*/
	off_t line_data_begin = d_offset + ((off_t)(bytes_per_line_actual) * mouse_y_lines);
	off_t line_data_end   = std::min((line_data_begin + bytes_per_line_actual), (d_offset + d_length));
	
	if(!doc.show_ascii || mouse_x_px < ascii_text_x)
	{
		/* Mouse is left of ASCII area, return last byte of previous line. */
		return line_data_begin - 1;
	}
	
	mouse_x_px -= ascii_text_x;
	
	unsigned int char_offset = doc.hf_char_at_x(mouse_x_px);
	off_t clicked_offset     = line_data_begin + char_offset;
	
	if(clicked_offset < line_data_end)
	{
		/* Mouse is on a character. */
		return clicked_offset;
	}
	else{
		/* Mouse is beyond end of line, return last byte of this line. */
		return line_data_end - 1;
	}
}

REHex::DocumentCtrl::CommentRegion::CommentRegion(off_t c_offset, off_t c_length, const wxString &c_text, int indent_depth):
	c_offset(c_offset), c_length(c_length), c_text(c_text), final_descendant(NULL) { this->indent_depth = indent_depth; }

void REHex::DocumentCtrl::CommentRegion::update_lines(REHex::DocumentCtrl &doc, wxDC &dc)
{
	if(doc.doc->get_inline_comment_mode() == ICM_SHORT || doc.doc->get_inline_comment_mode() == ICM_SHORT_INDENT)
	{
		y_lines = 2 + indent_final;
		return;
	}
	
	unsigned int row_chars = doc.hf_char_at_x(doc.virtual_width - (2 * doc._indent_width(indent_depth))) - 1;
	if(row_chars == 0)
	{
		/* Zero columns of width. Probably still initialising. */
		this->y_lines = 1 + indent_final;
	}
	else{
		auto comment_lines = _format_text(c_text, row_chars);
		this->y_lines  = comment_lines.size() + 1 + indent_final;
	}
}

void REHex::DocumentCtrl::CommentRegion::draw(REHex::DocumentCtrl &doc, wxDC &dc, int x, int64_t y)
{
	draw_container(doc, dc, x, y);
	
	int indent_width = doc._indent_width(indent_depth);
	x += indent_width;
	
	dc.SetFont(*(doc.hex_font));
	
	unsigned int row_chars = doc.hf_char_at_x(doc.virtual_width - (2 * indent_width)) - 1;
	if(row_chars == 0)
	{
		/* Zero columns of width. Probably still initialising. */
		return;
	}
	
	auto lines = _format_text(c_text, row_chars);
	
	if((doc.doc->get_inline_comment_mode() == ICM_SHORT || doc.doc->get_inline_comment_mode() == ICM_SHORT_INDENT) && lines.size() > 1)
	{
		wxString &first_line = lines.front();
		if(first_line.length() < row_chars)
		{
			first_line += L"\u2026";
		}
		else{
			first_line.Last() = L'\u2026';
		}
		
		lines.erase(std::next(lines.begin()), lines.end());
	}
	
	{
		int box_x = x + (doc.hf_char_width() / 4);
		int box_y = y + (doc.hf_height / 4);
		
		unsigned int box_w = (doc.virtual_width - (indent_depth * doc.hf_char_width() * 2)) - (doc.hf_char_width() / 2);
		unsigned int box_h = (lines.size() * doc.hf_height) + (doc.hf_height / 2);
		
		dc.SetPen(wxPen((*active_palette)[Palette::PAL_NORMAL_TEXT_FG], 1));
		dc.SetBrush(wxBrush((*active_palette)[Palette::PAL_COMMENT_BG]));
		
		dc.DrawRectangle(box_x, box_y, box_w, box_h);
		
		if(final_descendant != NULL)
		{
			dc.DrawLine(box_x, (box_y + box_h), box_x, (box_y + box_h + doc.hf_height));
			dc.DrawLine((box_x + box_w - 1), (box_y + box_h), (box_x + box_w - 1), (box_y + box_h + doc.hf_height));
		}
	}
	
	y += doc.hf_height / 2;
	
	dc.SetTextForeground((*active_palette)[Palette::PAL_COMMENT_FG]);
	dc.SetBackgroundMode(wxTRANSPARENT);
	
	for(auto li = lines.begin(); li != lines.end(); ++li)
	{
		dc.DrawText(*li, (x + (doc.hf_char_width() / 2)), y);
		y += doc.hf_height;
	}
}

wxCursor REHex::DocumentCtrl::CommentRegion::cursor_for_point(REHex::DocumentCtrl &doc, int x, int64_t y_lines, int y_px)
{
	int hf_width = doc.hf_char_width();
	int indent_width = doc._indent_width(indent_depth);
	
	if(
		(y_lines > 0 || y_px >= (doc.hf_height / 4)) /* Not above top edge. */
		&& (y_lines < (this->y_lines - 1) || y_px <= ((doc.hf_height / 4) * 3)) /* Not below bottom edge. */
		&& x >= (indent_width + (hf_width / 4)) /* Not left of left edge. */
		&& x < ((doc.virtual_width - (hf_width / 4)) - indent_width)) /* Not right of right edge. */
	{
		return wxCursor(wxCURSOR_HAND);
	}
	else{
		return wxNullCursor;
	}
}
