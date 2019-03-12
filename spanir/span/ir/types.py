#!/usr/bin/env python3

# MIT License
# Copyright (c) 2019 Anshuman Dhuliya

"""All value types available in the IR."""

# TODO: make all Type objects immutable. Till then assume immutable.

import logging
_log = logging.getLogger(__name__)
from typing import TypeVar, List

from span.util.logger import LS
from span.util.messages import PTR_INDLEV_INVALID

################################################
#BOUND START: useful_types
################################################

VarNameT = str
FuncNameT = str
LabelNameT = str
StructNameT = str
UnionNameT = str
FieldNameT = str  # a structure field name

TypeCodeT = int

OpSymbolT = str
OpNameT = str

NumericT = TypeVar('NumericT', int, float)

CfgNodeId = int
BasicBlockId = int
EdgeLabelT = str

# edge labels
FalseEdge: EdgeLabelT = "FalseEdge"  # False edge
TrueEdge: EdgeLabelT = "TrueEdge"  # True edge
UnCondEdge: EdgeLabelT = "UnCondEdge"  # Unconditional edge

# source location line:col given by user (only used to communicate back)
# this is a 64 bit integer
SourceLocationT = int

################################################
#BOUND END  : useful_types
################################################

################################################
#BOUND START: type_codes
################################################

# the order and ascending sequence is important
VOID_TC:TypeCodeT      = 0

INT1_TC:TypeCodeT      = 10  # bool
INT8_TC:TypeCodeT      = 11  # char
INT16_TC:TypeCodeT     = 12  # short
INT32_TC:TypeCodeT     = 13  # int
INT64_TC:TypeCodeT     = 14  # long long
INT128_TC:TypeCodeT    = 15  # ??

UINT8_TC: TypeCodeT       = 20  # unsigned char
UINT16_TC: TypeCodeT      = 21  # unsigned short
UINT32_TC: TypeCodeT      = 22  # unsigned int
UINT64_TC: TypeCodeT      = 23  # unsigned long long
UINT128_TC: TypeCodeT     = 24  # ??

FLOAT16_TC: TypeCodeT     = 50  # ??
FLOAT32_TC: TypeCodeT     = 51  # Float
FLOAT64_TC: TypeCodeT     = 52  # Double
FLOAT80_TC: TypeCodeT     = 53  # ??
FLOAT128_TC: TypeCodeT    = 54  # ??

PTR_TC: TypeCodeT         = 100  # pointer type code
FUNC_TC: TypeCodeT        = 200  # function type code
FUNC_SIG_TC: TypeCodeT    = 201
STRUCT_TC: TypeCodeT      = 300  # structure type code
STRUCT_SIG_TC: TypeCodeT  = 301  # structure type code
UNION_TC: TypeCodeT       = 400  # union type code
UNION_SIG_TC: TypeCodeT   = 401  # union type code


################################################
#BOUND END  : type_codes
################################################

class AnyT(object):
  """Base class for all static types.

  This class and all sub-classes suffixed with `T`,
  should not be instantiated.
  All classes suffixed with 'T' are only used for static and dynamic type
  checking.
  """
  def __init__(self) -> None:
    raise TypeError("Classes suffixed with 'T' shouldn't be instantiated."
                    "\nThese classes are exclusively used for static and "
                    "dynamic type checking.")

  def __eq__(self, other):
    if not isinstance(other, self.__class__):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    return True

  def __hash__(self): return hash(self.__class__.__name__)

  def __str__(self): return f"AnyT: {self.__class__.__name__}"

  def __repr__(self): return self.__str__()

class NilT(AnyT):
  def __init__(self): super().__init__()

class _Nil(NilT):
  def __init__(self): pass  # don't call super().__init__()

  def __eq__(self, other):
    if not isinstance(other, NilT):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    return True # all objects are equal

Nil = _Nil()

class Type(AnyT):
  """Class to represent types and super class for all compound types."""
  def __init__(self,
               typeCode: TypeCodeT
  ) -> None: # don't call super().__init__()
    self.typeCode = typeCode

  def isInteger(self):
    return INT1_TC <= self.typeCode <= UINT128_TC

  def isUnsigned(self):
    return UINT8_TC <= self.typeCode <= UINT128_TC

  def isFloat(self):
    return FLOAT16_TC <= self.typeCode <= FLOAT128_TC

  def isNumeric(self):
    return INT1_TC <= self.typeCode <= FLOAT128_TC

  def isPointer(self):
    return self.typeCode == PTR_TC

  def isFunc(self):
    return self.typeCode == FUNC_TC

  def isStuct(self):
    return self.typeCode == STRUCT_TC

  def isVoid(self):
    return self.typeCode == VOID_TC

  def __eq__(self,
             other: 'Type'
  ) -> bool:
    if not isinstance(other, Type):
      if LS: _log.warning("%s, %s are incomparable", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    return True

  def __hash__(self) -> int: return self.typeCode

  def __str__(self) -> str:
    ss = ""
    tc = self.typeCode

    if   tc == VOID_TC:       ss = "VOID"
    elif tc == INT1_TC:       ss = "INT1"
    elif tc == INT8_TC:       ss = "INT8"
    elif tc == INT16_TC:      ss = "INT16"
    elif tc == INT32_TC:      ss = "INT32"
    elif tc == INT64_TC:      ss = "INT64"
    elif tc == INT128_TC:     ss = "INT128"
    elif tc == UINT8_TC:      ss = "UINT8"
    elif tc == UINT16_TC:     ss = "UINT16"
    elif tc == UINT32_TC:     ss = "UINT32"
    elif tc == UINT64_TC:     ss = "UINT64"
    elif tc == UINT128_TC:    ss = "UINT128"
    elif tc == FLOAT16_TC:    ss = "FLOAT16"
    elif tc == FLOAT32_TC:    ss = "FLOAT32"
    elif tc == FLOAT64_TC:    ss = "FLOAT64"
    elif tc == FLOAT80_TC:    ss = "FLOAT80"
    elif tc == FLOAT128_TC:   ss = "FLOAT128"
    elif tc == PTR_TC:        ss = "PTR"
    elif tc == FUNC_TC:       ss = "FUNC"
    elif tc == FUNC_SIG_TC:   ss = "FUNC_SIG"
    elif tc == STRUCT_TC:     ss = "STRUCT"
    elif tc == STRUCT_SIG_TC: ss = "STRUCT_SIG"
    elif tc == UNION_TC:      ss = "UNION"
    elif tc == UNION_SIG_TC:  ss = "UNION_SIG"

    return ss

  def __repr__(self) -> str: return self.__str__()

################################################
#BOUND START: basic_type_objects
################################################

Void = Type(VOID_TC)

Int1 = Type(INT1_TC)
Int8 = Type(INT8_TC)
Int16 = Type(INT16_TC)
Int32 = Type(INT32_TC)
Int64 = Type(INT64_TC)
Int128 = Type(INT128_TC)

UInt8 = Type(UINT8_TC)
UInt16 = Type(UINT16_TC)
UInt32 = Type(UINT32_TC)
UInt64 = Type(UINT64_TC)
UInt128 = Type(UINT128_TC)

Float16 = Type(FLOAT16_TC)
Float32 = Type(FLOAT32_TC)
Float64 = Type(FLOAT64_TC)
Float80 = Type(FLOAT80_TC)
Float128 = Type(FLOAT128_TC)

# for convenience
Int = Int32
Char = UInt8
Float = Float32
Double = Float64

################################################
#BOUND END  : basic_type_objects
################################################

class Ptr(Type):
  """Concrete Pointer type.

  Instantiate this class to denote pointer types.
  E.g. types.Prt(types.Char, 2) is a ptr-to-ptr-to-char
  """
  def __init__(self,
               to: Type,
               indir: int = 1
  ) -> None:
    super().__init__(PTR_TC)
    if indir < 1:
      if LS: _log.error(PTR_INDLEV_INVALID)
      assert False, PTR_INDLEV_INVALID
    # indirection level to the object
    self.indir = indir
    # type of the object pointed to
    self.to = to

    # correct a recursive pointer
    while isinstance(self.to, Ptr):
      self.indir += 1
      self.to = self.to.to

  def getPointeeType(self) -> Type:
    if self.indir > 1:
      return Ptr(self.to, self.indir-1)
    return self.to

  def __eq__(self,
             other: 'Ptr'
  ) -> bool:
    if not isinstance(other, Ptr):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    if not self.indir == other.indir:
      if LS: _log.warning("IndLev Differ: %s, %s", self, other)
      return False
    if not self.to == other.to:
      if LS: _log.warning("DestType Differs: %s, %s", self, other)
      return False
    return True

  def __hash__(self): return hash(self.to) * self.indir

  def __str__(self): return self.__repr__()

  def __repr__(self):
    return f"types.Ptr({self.to}, {self.indir})"

class FuncSig(Type):
  """A function signature."""
  def __init__(self,
               returnType: Type,
               paramTypes: List[Type]
  ) -> None:
    super().__init__(FUNC_SIG_TC)
    self.returnType = returnType
    self.paramTypes = paramTypes

  def __eq__(self,
             other: 'FuncSig'
  ) -> bool:
    if not isinstance(other, FuncSig):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    if not self.returnType == other.returnType:
      if LS: _log.warning("ReturnType Differs: %s, %s", self, other)
      return False
    if not self.paramTypes == other.paramTypes:
      if LS: _log.warning("ParamTypes Differ: %s, %s", self, other)
      return False
    return True

  def __hash__(self):
    hsh = hash(self.returnType)
    increment = 170
    for tp in self.paramTypes:
      hsh = hsh ^ (hash(tp) + increment)
      increment += 17
    return hsh

class Struct(Type):
  """A structure type."""
  def __init__(self,
               structName: StructNameT
  ) -> None:
    super().__init__(STRUCT_TC)
    self.structName = structName

  def __eq__(self,
             other: 'Struct'
  ) -> bool:
    if not isinstance(other, Struct):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    if not self.structName == other.structName:
      if LS: _log.warning("StructName Differs: %s, %s", self, other)
      return False
    return True

  def __hash__(self):
    return hash(self.structName) + STRUCT_TC

class StructSig(Type):
  """A structure type signature."""
  def __init__(self,
               fieldTypes: List[Type]
  ) -> None:
    super().__init__(STRUCT_SIG_TC)
    self.fieldTypes = fieldTypes

  def __eq__(self,
             other: 'StructSig'
  ) -> bool:
    if not isinstance(other, StructSig):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    if not self.fieldTypes == other.fieldTypes:
      if LS: _log.warning("FieldTypes Differ: %s, %s", self, other)
      return False
    return True

  def __hash__(self):
    hsh = 13
    increment = 130
    for ftp in self.fieldTypes:
      hsh = hsh ^ (hash(ftp) + increment)
      increment += 13
    return hsh

class Union(Type):
  """A union type."""
  def __init__(self,
               unionName: UnionNameT
  ) -> None:
    super().__init__(STRUCT_TC)
    self.unionName = unionName

  def __eq__(self,
             other: 'Union'
  ) -> bool:
    if not isinstance(other, Union):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Types Differ: %s, %s", self, other)
      return False
    if not self.unionName == other.unionName:
      if LS: _log.warning("UnionName Differs: %s, %s", self, other)
      return False
    return True

  def __hash__(self):
    return hash(self.unionName) + STRUCT_TC

class UnionSig(Type):
  """A union type signature."""
  def __init__(self,
               fieldTypes: List[Type]
  ) -> None:
    super().__init__(UNION_SIG_TC)
    self.fieldTypes = fieldTypes

  def __eq__(self,
             other: 'UnionSig'
  ) -> bool:
    if not isinstance(other, UnionSig):
      if LS: _log.warning("%s, %s are incomparable.", self, other)
      return False
    if not self.typeCode == other.typeCode:
      if LS: _log.warning("Type Differs: %s, %s", self, other)
      return False
    if not self.fieldTypes == other.fieldTypes:
      if LS: _log.warning("FieldTypes Differ: %s, %s", self, other)
      return False
    return True

  def __hash__(self):
    hsh = 19
    increment = 190
    for ftp in self.fieldTypes:
      hsh = hsh ^ (hash(ftp) + increment)
      increment += 19
    return hsh
