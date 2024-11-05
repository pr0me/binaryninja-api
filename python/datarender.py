# Copyright (c) 2015-2024 Vector 35 Inc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import traceback
import ctypes

import binaryninja
from . import _binaryninjacore as core
from . import filemetadata
from . import binaryview
from . import function
from . import enums
from .log import log_error
from . import types
from . import highlight
from . import types


class TypeContext:
	def __init__(self, _type, _offset):
		self._type = _type
		self._offset = _offset

	@property
	def type(self):
		"""The Type object for the current context record"""
		return self._type

	@property
	def offset(self):
		"""The offset into the given type object"""
		return self._offset


class DataRenderer:
	"""
	DataRenderer objects tell the Linear View how to render specific types.

	The `perform_is_valid_for_data` method returns a boolean to indicate if your derived class
	is able to render the type, given the `addr` and `context`. The `context` is a list of Type
	objects which represents the chain of nested objects that is being displayed.

	The `perform_get_lines_for_data` method returns a list of `DisassemblyTextLine` objects each one
	representing a single line of Linear View output. The `prefix` variable is a list of `InstructionTextToken`'s
	which have already been generated by other `DataRenderer`'s.

	After defining the `DataRenderer` subclass you must then register it with the core. This is done by calling
	either `register_type_specific` or `register_generic`. A "generic" type renderer is able to be overridden by
	a "type specific" renderer. For instance there is a generic struct render which renders any struct that hasn't
	been explicitly overridden by a "type specific" renderer.

	In the below example we create a data renderer that overrides the default display for `struct BAR`::

		class BarDataRenderer(DataRenderer):
			def __init__(self):
				DataRenderer.__init__(self)
			def perform_is_valid_for_data(self, ctxt, view, addr, type, context):
				return DataRenderer.is_type_of_struct_name(type, "BAR", context)
			def perform_get_lines_for_data(self, ctxt, view, addr, type, prefix, width, context, language):
				prefix.append(InstructionTextToken(InstructionTextTokenType.TextToken, "I'm in ur BAR"))
				return [DisassemblyTextLine(prefix, addr)]
			def __del__(self):
				pass

		BarDataRenderer().register_type_specific()

	Note that the formatting is sub-optimal to work around an issue with Sphinx and reStructured text
	"""
	_registered_renderers = []

	def __init__(self, context=None):
		self._cb = core.BNCustomDataRenderer()
		self._cb.context = context
		self._cb.freeObject = self._cb.freeObject.__class__(self._free_object)
		self._cb.isValidForData = self._cb.isValidForData.__class__(self._is_valid_for_data)
		self._cb.getLinesForData = self._cb.getLinesForData.__class__(self._get_lines_for_data)
		self._cb.freeLines = self._cb.freeLines.__class__(self._free_lines)
		self.handle = core.BNCreateDataRenderer(self._cb)

	@staticmethod
	def is_type_of_struct_name(t, name, context):
		return (
		    t.type_class == enums.TypeClass.StructureTypeClass and len(context) > 0
		    and isinstance(context[-1].type, types.NamedTypeReferenceType) and context[-1].type.name == name
		)

	def register_type_specific(self):
		core.BNRegisterTypeSpecificDataRenderer(core.BNGetDataRendererContainer(), self.handle)
		self.__class__._registered_renderers.append(self)

	def register_generic(self):
		core.BNRegisterGenericDataRenderer(core.BNGetDataRendererContainer(), self.handle)
		self.__class__._registered_renderers.append(self)

	def _free_object(self, ctxt):
		try:
			self.perform_free_object(ctxt)
		except:
			log_error(traceback.format_exc())

	def _is_valid_for_data(self, ctxt, view, addr, type, context, ctxCount):
		try:
			file_metadata = filemetadata.FileMetadata(handle=core.BNGetFileForView(view))
			view = binaryview.BinaryView(file_metadata=file_metadata, handle=core.BNNewViewReference(view))
			type = types.Type.create(handle=core.BNNewTypeReference(type))
			pycontext = []
			for i in range(0, ctxCount):
				pycontext.append(
				    TypeContext(types.Type.create(core.BNNewTypeReference(context[i].type)), context[i].offset)
				)
			return self.perform_is_valid_for_data(ctxt, view, addr, type, pycontext)
		except:
			log_error(traceback.format_exc())
			return False

	def _get_lines_for_data(self, ctxt, view, addr, type, prefix, prefixCount, width, count, typeCtx, ctxCount, language):
		try:
			file_metadata = filemetadata.FileMetadata(handle=core.BNGetFileForView(view))
			view = binaryview.BinaryView(file_metadata=file_metadata, handle=core.BNNewViewReference(view))
			type = types.Type.create(handle=core.BNNewTypeReference(type))

			prefixTokens = function.InstructionTextToken._from_core_struct(prefix, prefixCount)
			pycontext = []
			for i in range(ctxCount):
				pycontext.append(
				    TypeContext(types.Type.create(core.BNNewTypeReference(typeCtx[i].type)), typeCtx[i].offset)
				)

			result = self.perform_get_lines_for_data(ctxt, view, addr, type, prefixTokens, width, pycontext, language)

			count[0] = len(result)
			self.line_buf = (core.BNDisassemblyTextLine * len(result))()
			for i in range(len(result)):
				line = result[i]
				color = line.highlight
				if not isinstance(color,
				                  enums.HighlightStandardColor) and not isinstance(color, highlight.HighlightColor):
					raise ValueError("Specified color is not one of HighlightStandardColor, highlight.HighlightColor")
				if isinstance(color, enums.HighlightStandardColor):
					color = highlight.HighlightColor(color)
				self.line_buf[i].highlight = color._to_core_struct()
				if line.address is None:
					if len(line.tokens) > 0:
						self.line_buf[i].addr = line.tokens[0].address
					else:
						self.line_buf[i].addr = 0
				else:
					self.line_buf[i].addr = line.address
				if line.il_instruction is not None:
					self.line_buf[i].instrIndex = line.il_instruction.instr_index
				else:
					self.line_buf[i].instrIndex = 0xffffffffffffffff

				self.line_buf[i].count = len(line.tokens)
				self.line_buf[i].tokens = function.InstructionTextToken._get_core_struct(line.tokens)

			return ctypes.cast(self.line_buf, ctypes.c_void_p).value
		except:
			log_error(traceback.format_exc())
			return None

	def _free_lines(self, ctxt, lines, count):
		self.line_buf = None

	def perform_free_object(self, ctxt):
		pass

	def perform_is_valid_for_data(self, ctxt, view, addr, type, context):
		return False

	def perform_get_lines_for_data(self, ctxt, view, addr, type, prefix, width, context, language):
		return []

	def __del__(self):
		pass
