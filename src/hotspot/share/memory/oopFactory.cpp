/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "memory/oopFactory.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/instanceOop.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "oops/valueKlass.hpp"
#include "oops/valueArrayKlass.hpp"
#include "runtime/handles.inline.hpp"


typeArrayOop oopFactory::new_charArray(const char* utf8_str, TRAPS) {
  int length = utf8_str == NULL ? 0 : UTF8::unicode_length(utf8_str);
  typeArrayOop result = new_charArray(length, CHECK_NULL);
  if (length > 0) {
    UTF8::convert_to_unicode(utf8_str, result->char_at_addr(0), length);
  }
  return result;
}

typeArrayOop oopFactory::new_tenured_charArray(int length, TRAPS) {
  return TypeArrayKlass::cast(Universe::charArrayKlassObj())->allocate(length, THREAD);
}

typeArrayOop oopFactory::new_typeArray(BasicType type, int length, TRAPS) {
  Klass* type_asKlassOop = Universe::typeArrayKlassObj(type);
  TypeArrayKlass* type_asArrayKlass = TypeArrayKlass::cast(type_asKlassOop);
  typeArrayOop result = type_asArrayKlass->allocate(length, THREAD);
  return result;
}

// Create a Java array that points to Symbol.
// As far as Java code is concerned, a Symbol array is either an array of
// int or long depending on pointer size.  Only stack trace elements in Throwable use
// this.  They cast Symbol* into this type.
typeArrayOop oopFactory::new_symbolArray(int length, TRAPS) {
  BasicType type = LP64_ONLY(T_LONG) NOT_LP64(T_INT);
  Klass* type_asKlassOop = Universe::typeArrayKlassObj(type);
  TypeArrayKlass* type_asArrayKlass = TypeArrayKlass::cast(type_asKlassOop);
  typeArrayOop result = type_asArrayKlass->allocate(length, THREAD);
  return result;
}

typeArrayOop oopFactory::new_typeArray_nozero(BasicType type, int length, TRAPS) {
  Klass* type_asKlassOop = Universe::typeArrayKlassObj(type);
  TypeArrayKlass* type_asArrayKlass = TypeArrayKlass::cast(type_asKlassOop);
  typeArrayOop result = type_asArrayKlass->allocate_common(length, false, THREAD);
  return result;
}


objArrayOop oopFactory::new_objArray(Klass* klass, int length, TRAPS) {
  assert(klass->is_klass(), "must be instance class");
  assert(!klass->is_value() || (!ValueKlass::cast(klass)->flatten_array()),
           "Did not expect flatten array of value klass");
  if (klass->is_array_klass()) {
    return ArrayKlass::cast(klass)->allocate_arrayArray(1, length, THREAD);
  } else {
    return InstanceKlass::cast(klass)->allocate_objArray(1, length, THREAD);
  }
}

arrayOop oopFactory::new_valueArray(Klass* klass, int length, TRAPS) {
  assert(klass->is_value(), "Klass must be value type");
  Klass* array_klass = klass->array_klass(CHECK_NULL); // Flat value array or object array ?
  assert(array_klass->is_valueArray_klass() || array_klass->is_objArray_klass(),
         "Expect an array class here");

  if (array_klass->is_valueArray_klass()) {
    return (arrayOop) ValueArrayKlass::cast(array_klass)->allocate(length, THREAD);
  }

  ValueKlass* vklass = ValueKlass::cast(klass);
  objArrayOop array = oopFactory::new_objArray(klass, length, CHECK_NULL);
  if (length == 0) {
    return array;
  }

  // Populate default values...
  objArrayHandle array_h(THREAD, array);
  instanceOop value = (instanceOop)vklass->default_value();
  for (int i = 0; i < length; i++) {
    array_h->obj_at_put(i, value);
  }
  return array_h();
}

arrayOop oopFactory::new_array(Klass* klass, int length, TRAPS) {
  return (klass->is_value()) ? new_valueArray(klass, length, THREAD) :
      (arrayOop)new_objArray(klass, length, THREAD);
}

objArrayHandle oopFactory::new_objArray_handle(Klass* klass, int length, TRAPS) {
  objArrayOop obj = new_objArray(klass, length, CHECK_(objArrayHandle()));
  return objArrayHandle(THREAD, obj);
}

typeArrayHandle oopFactory::new_byteArray_handle(int length, TRAPS) {
  typeArrayOop obj = new_byteArray(length, CHECK_(typeArrayHandle()));
  return typeArrayHandle(THREAD, obj);
}
