/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "LayoutEngine.h"
#include "LayoutBlockBoxSpace.h"
#include "LayoutDetails.h"
#include "LayoutInlineBoxText.h"
#include "LayoutTable.h"
#include "Pool.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/Types.h"
#include <cstddef>
#include <float.h>

namespace Rml {

#define MAX(a, b) (a > b ? a : b)

struct LayoutChunk
{
	static const unsigned int size = MAX(sizeof(LayoutBlockBox), MAX(sizeof(LayoutInlineBox), MAX(sizeof(LayoutInlineBoxText), MAX(sizeof(LayoutLineBox), sizeof(LayoutBlockBoxSpace)))));
	alignas(std::max_align_t) char buffer[size];
};

static Pool< LayoutChunk > layout_chunk_pool(200, true);

// Formats the contents for a root-level element (usually a document or floating element).
void LayoutEngine::FormatElement(Element* element, Vector2f containing_block, const Box* override_initial_box, Vector2f* out_visible_overflow_size)
{
	RMLUI_ASSERT(element && containing_block.x >= 0 && containing_block.y >= 0);
#ifdef RMLUI_ENABLE_PROFILING
	RMLUI_ZoneScopedC(0xB22222);
	auto name = CreateString(80, "%s %x", element->GetAddress(false, false).c_str(), element);
	RMLUI_ZoneName(name.c_str(), name.size());
#endif

	LayoutBlockBox containing_block_box(nullptr, nullptr, Box(containing_block), 0.0f, FLT_MAX);

	Box box;
	if (override_initial_box)
		box = *override_initial_box;
	else
		LayoutDetails::BuildBox(box, containing_block, element, false);

	float min_height, max_height;
	LayoutDetails::GetMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing_block.y);

	LayoutBlockBox* block_context_box = containing_block_box.AddBlockElement(element, box, min_height, max_height);

	for (int layout_iteration = 0; layout_iteration < 2; layout_iteration++)
	{
		for (int i = 0; i < element->GetNumChildren(); i++)
		{
			if (!FormatElement(block_context_box, element->GetChild(i)))
				i = -1;
		}

		if (block_context_box->Close() == LayoutBlockBox::OK)
			break;
	}

	block_context_box->CloseAbsoluteElements();

	if (out_visible_overflow_size)
		*out_visible_overflow_size = block_context_box->GetVisibleOverflowSize();

	element->OnLayout();
}

void* LayoutEngine::AllocateLayoutChunk(size_t size)
{
	RMLUI_ASSERT(size <= LayoutChunk::size);
	(void)size;
	
	return layout_chunk_pool.AllocateAndConstruct();
}

void LayoutEngine::DeallocateLayoutChunk(void* chunk)
{
	layout_chunk_pool.DestroyAndDeallocate((LayoutChunk*) chunk);
}

// Positions a single element and its children within this layout.
bool LayoutEngine::FormatElement(LayoutBlockBox* block_context_box, Element* element)
{
#ifdef RMLUI_ENABLE_PROFILING
	RMLUI_ZoneScoped;
	auto name = CreateString(80, ">%s %x", element->GetAddress(false, false).c_str(), element);
	RMLUI_ZoneName(name.c_str(), name.size());
#endif

	auto& computed = element->GetComputedValues();

	// Check if we have to do any special formatting for any elements that don't fit into the standard layout scheme.
	if (FormatElementSpecial(block_context_box, element))
		return true;

	// Fetch the display property, and don't lay this element out if it is set to a display type of none.
	if (computed.display == Style::Display::None)
		return true;

	// Check for an absolute position; if this has been set, then we remove it from the flow and add it to the current
	// block box to be laid out and positioned once the block has been closed and sized.
	if (computed.position == Style::Position::Absolute || computed.position == Style::Position::Fixed)
	{
		// Display the element as a block element.
		block_context_box->AddAbsoluteElement(element);
		return true;
	}

	// If the element is floating, we remove it from the flow.
	if (computed.float_ != Style::Float::None)
	{
		LayoutEngine::FormatElement(element, LayoutDetails::GetContainingBlock(block_context_box));
		return block_context_box->AddFloatElement(element);
	}

	// The element is nothing exceptional, so we treat it as a normal block, inline or replaced element.
	switch (computed.display)
	{
		case Style::Display::Block:       return FormatElementBlock(block_context_box, element);
		case Style::Display::Inline:      return FormatElementInline(block_context_box, element);
		case Style::Display::InlineBlock: return FormatElementInlineBlock(block_context_box, element);
		case Style::Display::Table:       return FormatElementTable(block_context_box, element);

		case Style::Display::TableRow:
		case Style::Display::TableRowGroup:
		case Style::Display::TableColumn:
		case Style::Display::TableColumnGroup:
		case Style::Display::TableCell:
		{
			const Property* display_property = element->GetProperty(PropertyId::Display);
			Log::Message(Log::LT_WARNING, "Element has a display type '%s', but is not located in a table. It will not be formatted. In element %s",
				display_property ? display_property->ToString().c_str() : "*unknown*",
				element->GetAddress().c_str()
			);
			return true;
		}
		case Style::Display::None:        RMLUI_ERROR; /* handled above */ break;
	}

	return true;
}

// Formats and positions an element as a block element.
bool LayoutEngine::FormatElementBlock(LayoutBlockBox* block_context_box, Element* element)
{
	RMLUI_ZoneScopedC(0x2F4F4F);

	Box box;
	float min_height, max_height;
	LayoutDetails::BuildBox(box, min_height, max_height, block_context_box, element, false);

	LayoutBlockBox* new_block_context_box = block_context_box->AddBlockElement(element, box, min_height, max_height);
	if (new_block_context_box == nullptr)
		return false;

	// Format the element's children.
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (!FormatElement(new_block_context_box, element->GetChild(i)))
			i = -1;
	}

	// Close the block box, and check the return code; we may have overflowed either this element or our parent.
	switch (new_block_context_box->Close())
	{
		// We need to reformat ourself; format all of our children again and close the box. No need to check for error
		// codes, as we already have our vertical slider bar.
		case LayoutBlockBox::LAYOUT_SELF:
		{
			for (int i = 0; i < element->GetNumChildren(); i++)
				FormatElement(new_block_context_box, element->GetChild(i));

			if (new_block_context_box->Close() == LayoutBlockBox::OK)
			{
				element->OnLayout();
				break;
			}
		}
		//-fallthrough
		// We caused our parent to add a vertical scrollbar; bail out!
		case LayoutBlockBox::LAYOUT_PARENT:
		{
			return false;
		}
		break;

		default:
			element->OnLayout();
	}

	return true;
}

// Formats and positions an element as an inline element.
bool LayoutEngine::FormatElementInline(LayoutBlockBox* block_context_box, Element* element)
{
	RMLUI_ZoneScopedC(0x3F6F6F);

	const Vector2f containing_block = LayoutDetails::GetContainingBlock(block_context_box);

	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, true);
	LayoutInlineBox* inline_box = block_context_box->AddInlineElement(element, box);

	// Format the element's children.
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		if (!FormatElement(block_context_box, element->GetChild(i)))
			return false;
	}

	inline_box->Close();

	return true;
}

// Positions an element as a sized inline element, formatting its internal hierarchy as a block element.
bool LayoutEngine::FormatElementInlineBlock(LayoutBlockBox* block_context_box, Element* element)
{
	RMLUI_ZoneScopedC(0x1F2F2F);

	// Format the element separately as a block element, then position it inside our own layout as an inline element.
	Vector2f containing_block_size = LayoutDetails::GetContainingBlock(block_context_box);

	FormatElement(element, containing_block_size);

	block_context_box->AddInlineElement(element, element->GetBox())->Close();

	return true;
}


bool LayoutEngine::FormatElementTable(LayoutBlockBox* block_context_box, Element* element_table)
{
	LayoutBlockBox* table_block_context_box = nullptr;
	
	{
		Box box;
		float min_height, max_height;
		LayoutDetails::BuildBox(box, min_height, max_height, block_context_box, element_table, false);

		table_block_context_box = block_context_box->AddBlockElement(element_table, box, min_height, max_height);
	}

	if (!table_block_context_box)
		return false;

	for (int i = 0; i < 2; i++)
	{
		LayoutBlockBox::CloseResult result = LayoutTable::FormatTable(table_block_context_box, element_table);

		// If the close failed, it probably means that the table or its parent produced scrollbars. Try again, but only once.
		if (result == LayoutBlockBox::LAYOUT_SELF)
			continue;
		else if (result == LayoutBlockBox::LAYOUT_PARENT)
			return false;
		else if (result == LayoutBlockBox::OK)
			break;
	}

	return true;
}

// Executes any special formatting for special elements.
bool LayoutEngine::FormatElementSpecial(LayoutBlockBox* block_context_box, Element* element)
{
	static const String br("br");
	
	// Check for a <br> tag.
	if (element->GetTagName() == br)
	{
		block_context_box->AddBreak();
		element->OnLayout();
		return true;
	}

	return false;
}

} // namespace Rml
