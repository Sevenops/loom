/*
 * Copyright (c) 1998, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "code/scopeDesc.hpp"
#include "compiler/oopMap.hpp"
#include "compiler/oopMap.inline.hpp"
#include "compiler/oopMapStubGenerator.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/signature.hpp"
#include "utilities/align.hpp"
#ifdef COMPILER1
#include "c1/c1_Defs.hpp"
#endif
#ifdef COMPILER2
#include "opto/optoreg.hpp"
#endif

// OopMapStream

OopMapStream::OopMapStream(const OopMap* oop_map, int oop_types_mask)
  : _stream(oop_map->write_stream()->buffer()) {
  // _stream = new CompressedReadStream();
  _mask = oop_types_mask;
  _size = oop_map->omv_count();
  _position = 0;
  _valid_omv = false;
}

OopMapStream::OopMapStream(const ImmutableOopMap* oop_map, int oop_types_mask)
  : _stream(oop_map->data_addr()) {
  // _stream = new CompressedReadStream(oop_map->data_addr());
  _mask = oop_types_mask;
  _size = oop_map->count();
  _position = 0;
  _valid_omv = false;
}

void OopMapStream::find_next() {
  while(_position++ < _size) {
    _omv.read_from(&_stream);
    if(((int)_omv.type() & _mask) > 0) {
      _valid_omv = true;
      return;
    }
  }
  _valid_omv = false;
}


// OopMap

// frame_size units are stack-slots (4 bytes) NOT intptr_t; we can name odd
// slots to hold 4-byte values like ints and floats in the LP64 build.
OopMap::OopMap(int frame_size, int arg_count) {
  // OopMaps are usually quite so small, so pick a small initial size
  set_write_stream(new CompressedWriteStream(32));
  set_omv_count(0);
  _num_oops = 0;
  _index = -1;

#ifdef ASSERT
  _locs_length = VMRegImpl::stack2reg(0)->value() + frame_size + arg_count;
  _locs_used   = NEW_RESOURCE_ARRAY(OopMapValue::oop_types, _locs_length);
  for(int i = 0; i < _locs_length; i++) _locs_used[i] = OopMapValue::unused_value;
#endif
}


OopMap::OopMap(OopMap::DeepCopyToken, OopMap* source) {
  // This constructor does a deep copy
  // of the source OopMap.
  set_write_stream(new CompressedWriteStream(source->omv_count() * 2));
  set_omv_count(0);
  set_offset(source->offset());
  _num_oops = source->num_oops();
  _index = -1;

#ifdef ASSERT
  _locs_length = source->_locs_length;
  _locs_used = NEW_RESOURCE_ARRAY(OopMapValue::oop_types, _locs_length);
  for(int i = 0; i < _locs_length; i++) _locs_used[i] = OopMapValue::unused_value;
#endif

  // We need to copy the entries too.
  for (OopMapStream oms(source); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    omv.write_on(write_stream());
    increment_count();
  }
}


OopMap* OopMap::deep_copy() {
  return new OopMap(_deep_copy_token, this);
}

void OopMap::copy_data_to(address addr) const {
  memcpy(addr, write_stream()->buffer(), write_stream()->position());
}

class OopMapSort {
private:
  const OopMap* _map;
  OopMapValue* _values;
  int _count;

public:
  OopMapSort(const OopMap* map) : _map(map), _count(0) {
    _values = NEW_RESOURCE_ARRAY(OopMapValue, _map->omv_count());
  }

  void sort();

  void print();

  void write(CompressedWriteStream* stream) {
    for (int i = 0; i < _count; ++i) {
      _values[i].write_on(stream);
    }
  }

private:
  int find_derived_position(OopMapValue omv, int start) {
    assert(omv.type() == OopMapValue::derived_oop_value, "");

    VMReg base = omv.content_reg();
    int i = start;

    for (; i < _count; ++i) {
      if (base == _values[i].reg()) {

        for (int n = i + 1; n < _count; ++n) {
          if (_values[i].type() != OopMapValue::derived_oop_value || _values[i].content_reg() != base) {
            return n;
          }

          if (derived_cost(_values[i]) > derived_cost(omv)) {
            return n;
          }
        }
        return _count;
      }
    }

    assert(false, "failed to find base");
    return -1;
  }

  int find_position(OopMapValue omv, int start) {
    assert(omv.type() != OopMapValue::derived_oop_value, "");

    int i = start;
    for (; i < _count; ++i) {
      if (omv_cost(_values[i]) > omv_cost(omv)) {
        return i;
      }
    }
    assert(i < _map->omv_count(), "bounds check");
    return i;
  }

  void insert(OopMapValue value, int pos) {
    assert(pos >= 0 && pos < _map->omv_count(), "bounds check");
    assert(pos <= _count, "sanity");

    if (pos < _count) {
      OopMapValue prev = _values[pos];

      for (int i = pos; i < _count; ++i) {
        OopMapValue tmp = _values[i+1];
        _values[i+1] = prev;
        prev = tmp;
      }
    }
    _values[pos] = value;

    ++_count;
  }

  int omv_cost(OopMapValue omv) {
    assert(omv.type() == OopMapValue::oop_value || omv.type() == OopMapValue::narrowoop_value, "");
    return reg_cost(omv.reg());
  }

  int reg_cost(VMReg reg) {
    if (reg->is_reg()) {
      return 0;
    }
    return reg->reg2stack() * VMRegImpl::stack_slot_size;
  }

  int derived_cost(OopMapValue omv) {
    return reg_cost(omv.reg());
  }
};

void OopMapSort::sort() {
  for (OopMapStream oms(_map); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    assert(omv.type() == OopMapValue::oop_value || omv.type() == OopMapValue::narrowoop_value || omv.type() == OopMapValue::derived_oop_value || omv.type() == OopMapValue::callee_saved_value, "");
  }

  for (OopMapStream oms(_map, OopMapValue::callee_saved_value); !oms.is_done(); oms.next()) {
    insert(oms.current(), _count);
  }

  int start = _count;
  for (OopMapStream oms(_map, OopMapValue::oop_value | OopMapValue::narrowoop_value); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    int pos = find_position(omv, start);
    insert(omv, pos);
  }

  for (OopMapStream oms(_map, OopMapValue::derived_oop_value); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    int pos = find_derived_position(omv, start);
    assert(pos > 0, "");
    insert(omv, pos);
  }
}

void OopMapSort::print() {
  for (int i = 0; i < _count; ++i) {
    OopMapValue omv = _values[i];
    if (omv.type() == OopMapValue::oop_value || omv.type() == OopMapValue::narrowoop_value) {
      if (omv.reg()->is_reg()) {
        tty->print_cr("[%c][%d] -> reg (%ld)", omv.type() == OopMapValue::narrowoop_value ? 'n' : 'o', i, omv.reg()->value());
      } else {
        tty->print_cr("[%c][%d] -> stack (%lx)", omv.type() == OopMapValue::narrowoop_value ? 'n' : 'o', i, omv.reg()->reg2stack() * VMRegImpl::stack_slot_size);
      }
    } else {
      if (omv.content_reg()->is_reg()) {
        tty->print_cr("[d][%d] -> reg (%ld) stack (%lx)", i, omv.content_reg()->value(), omv.reg()->reg2stack() * VMRegImpl::stack_slot_size);
      } else if (omv.reg()->is_reg()) {
        tty->print_cr("[d][%d] -> stack (%lx) reg (%ld)", i, omv.content_reg()->reg2stack() * VMRegImpl::stack_slot_size, omv.reg()->value());
      } else {
        int derived_offset = omv.reg()->reg2stack() * VMRegImpl::stack_slot_size;
        int base_offset = omv.content_reg()->reg2stack() * VMRegImpl::stack_slot_size;
        tty->print_cr("[d][%d] -> stack (%x) stack (%x)", i, base_offset, derived_offset);
      }
    }
  }
}

void OopMap::copy_and_sort_data_to(address addr) const {
  OopMapSort sort(this);
  sort.sort();
  CompressedWriteStream* stream = new CompressedWriteStream(_write_stream->position());
  sort.write(stream);

  assert(stream->position() == write_stream()->position(), "");
  memcpy(addr, stream->buffer(), stream->position());
  //copy_data_to(addr);
  //sort.print();
}

int OopMap::heap_size() const {
  int size = sizeof(OopMap);
  int align = sizeof(void *) - 1;
  size += write_stream()->position();
  // Align to a reasonable ending point
  size = ((size+align) & ~align);
  return size;
}

// frame_size units are stack-slots (4 bytes) NOT intptr_t; we can name odd
// slots to hold 4-byte values like ints and floats in the LP64 build.
void OopMap::set_xxx(VMReg reg, OopMapValue::oop_types x, VMReg optional) {

  assert(reg->value() < _locs_length, "too big reg value for stack size");
  assert( _locs_used[reg->value()] == OopMapValue::unused_value, "cannot insert twice" );
  debug_only( _locs_used[reg->value()] = x; )

  OopMapValue o(reg, x);

  if(x == OopMapValue::callee_saved_value) {
    // This can never be a stack location, so we don't need to transform it.
    assert(optional->is_reg(), "Trying to callee save a stack location");
    o.set_content_reg(optional);
  } else if(x == OopMapValue::derived_oop_value) {
    o.set_content_reg(optional);
  }

  o.write_on(write_stream());
  increment_count();
  if (x == OopMapValue::oop_value || x == OopMapValue::narrowoop_value)
    increment_num_oops();
}


void OopMap::set_oop(VMReg reg) {
  set_xxx(reg, OopMapValue::oop_value, VMRegImpl::Bad());
}


void OopMap::set_value(VMReg reg) {
  // At this time, we don't need value entries in our OopMap.
  // set_xxx(reg, OopMapValue::live_value, VMRegImpl::Bad());
}


void OopMap::set_narrowoop(VMReg reg) {
  set_xxx(reg, OopMapValue::narrowoop_value, VMRegImpl::Bad());
}


void OopMap::set_callee_saved(VMReg reg, VMReg caller_machine_register ) {
  set_xxx(reg, OopMapValue::callee_saved_value, caller_machine_register);
}


void OopMap::set_derived_oop(VMReg reg, VMReg derived_from_local_register ) {
  if( reg == derived_from_local_register ) {
    // Actually an oop, derived shares storage with base,
    set_oop(reg);
  } else {
    set_xxx(reg, OopMapValue::derived_oop_value, derived_from_local_register);
  }
}

// OopMapSet

OopMapSet::OopMapSet() {
  set_om_size(MinOopMapAllocation);
  set_om_count(0);
  OopMap** temp = NEW_RESOURCE_ARRAY(OopMap*, om_size());
  set_om_data(temp);
}


void OopMapSet::grow_om_data() {
  int new_size = om_size() * 2;
  OopMap** new_data = NEW_RESOURCE_ARRAY(OopMap*, new_size);
  memcpy(new_data,om_data(),om_size() * sizeof(OopMap*));
  set_om_size(new_size);
  set_om_data(new_data);
}

int OopMapSet::add_gc_map(int pc_offset, OopMap *map ) {
  assert(om_size() != -1,"Cannot grow a fixed OopMapSet");

  if(om_count() >= om_size()) {
    grow_om_data();
  }
  map->set_offset(pc_offset);

#ifdef ASSERT
  if(om_count() > 0) {
    OopMap* last = at(om_count()-1);
    if (last->offset() == map->offset() ) {
      fatal("OopMap inserted twice");
    }
    if(last->offset() > map->offset()) {
      tty->print_cr( "WARNING, maps not sorted: pc[%d]=%d, pc[%d]=%d",
                      om_count(),last->offset(),om_count()+1,map->offset());
    }
  }
#endif // ASSERT

  int index = om_count();
  set(index,map);
  map->_index = index;
  increment_count();
  return index;
}


int OopMapSet::heap_size() const {
  // The space we use
  int size = sizeof(OopMap);
  int align = sizeof(void *) - 1;
  size = ((size+align) & ~align);
  size += om_count() * sizeof(OopMap*);

  // Now add in the space needed for the indivdiual OopMaps
  for(int i=0; i < om_count(); i++) {
    size += at(i)->heap_size();
  }
  // We don't need to align this, it will be naturally pointer aligned
  return size;
}


OopMap* OopMapSet::singular_oop_map() {
  guarantee(om_count() == 1, "Make sure we only have a single gc point");
  return at(0);
}

OopMap* OopMapSet::find_map_at_offset(int pc_offset) const {
  int i, len = om_count();
  assert( len > 0, "must have pointer maps" );

  // Scan through oopmaps. Stop when current offset is either equal or greater
  // than the one we are looking for.
  for( i = 0; i < len; i++) {
    if( at(i)->offset() >= pc_offset )
      break;
  }

  assert( i < len, "oopmap not found" );

  OopMap* m = at(i);
  assert( m->offset() == pc_offset, "oopmap not found" );
  return m;
}

void AddDerivedOop::do_derived_oop(oop* base, oop* derived) {
#if !defined(TIERED) && !INCLUDE_JVMCI
  COMPILER1_PRESENT(ShouldNotReachHere();)
#endif // !defined(TIERED) && !INCLUDE_JVMCI
#if COMPILER2_OR_JVMCI
  DerivedPointerTable::add(derived, base);
#endif // COMPILER2_OR_JVMCI
}



void OopMapSet::oops_do(const frame *fr, const RegisterMap* reg_map, OopClosure* f, DerivedOopClosure* df) {
  // add_derived_oop: add derived oops to a table
  find_map(fr)->oops_do(fr, reg_map, f, df);
  // all_do(fr, reg_map, f, df != NULL ? df : &add_derived_oop, &do_nothing_cl);
}

// void OopMapSet::all_do(const frame *fr, const RegisterMap *reg_map,
//                        OopClosure* oop_fn, DerivedOopClosure* derived_oop_fn,
//                        OopClosure* value_fn) {
//   find_map(fr)->oops_do(fr, reg_map, oop_fn, derived_oop_fn, value_fn);
// }

ExplodedOopMap::ExplodedOopMap(const ImmutableOopMap* oopMap) {
  _oopValues = copyOopMapValues(oopMap, OopMapValue::oop_value | OopMapValue::narrowoop_value, &_nrOopValues);
  _calleeSavedValues = copyOopMapValues(oopMap, OopMapValue::callee_saved_value, &_nrCalleeSavedValuesCount);
  _derivedValues = copyOopMapValues(oopMap, OopMapValue::derived_oop_value, &_nrDerivedValues);
}

OopMapValue* ExplodedOopMap::values(int mask) {
  if (mask == (OopMapValue::oop_value | OopMapValue::narrowoop_value)) {
    return _oopValues;
  } else if (mask == OopMapValue::callee_saved_value) {
    return _calleeSavedValues;
  } else if (mask == OopMapValue::derived_oop_value) {
    return _derivedValues;
  } else {
    guarantee(false, "new type?");
    return NULL;
  }
}

int ExplodedOopMap::count(int mask) {
  if (mask == (OopMapValue::oop_value | OopMapValue::narrowoop_value)) {
    return _nrOopValues;
  } else if (mask == OopMapValue::callee_saved_value) {
    return _nrCalleeSavedValuesCount;
  } else if (mask == OopMapValue::derived_oop_value) {
    return _nrDerivedValues;
  } else {
    guarantee(false, "new type?");
    return 0;
  }
}

OopMapValue* ExplodedOopMap::copyOopMapValues(const ImmutableOopMap* oopMap, int mask, int* nr) {
  OopMapValue omv;
  int count = 0;
  // We want coop and oop oop_types
  for (OopMapStream oms(oopMap,mask); !oms.is_done(); oms.next()) {
    ++count;
  }
  *nr = count;

  OopMapValue* values = (OopMapValue*) NEW_C_HEAP_ARRAY(unsigned char, sizeof(OopMapValue) * count, mtCode);

  int i = 0;
  for (OopMapStream oms(oopMap,mask); !oms.is_done(); oms.next()) {
    assert(i < count, "overflow");
    values[i] = oms.current();
    i++;
  }

  i = 0;
  for (OopMapStream oms(oopMap,mask); !oms.is_done(); oms.next()) {
    assert(i < count, "overflow");
    assert(values[i].equals(oms.current()), "must");
    i++;
  }

  return values;
}

// NULL, fail, success (address)
void ImmutableOopMap::generate_stub() const {
  /* The address of the ImmutableOopMap is put into the _freeze_stub and _thaw_stub 
   * if we can't generate the stub for some reason */
  if (_freeze_stub == NULL) {
    OopMapStubGenerator cgen(*this);
    if (Atomic::cmpxchg((address) this, &_freeze_stub, (address) NULL) == NULL) {
      if (!cgen.generate()) {
        Atomic::store((address) this, &_thaw_stub);
        cgen.free();
        return;
      }

      Atomic::store(cgen.freeze_stub(), &_freeze_stub);
      Atomic::store(cgen.thaw_stub(), &_thaw_stub);
    }
  }
}

void ImmutableOopMap::oops_do(const frame *fr, const RegisterMap *reg_map,
                              OopClosure* oop_fn, DerivedOopClosure* derived_oop_fn) const {
  AddDerivedOop add_derived_oop;
  if (derived_oop_fn == NULL) {
    derived_oop_fn = &add_derived_oop;
  }
  OopMapDo<OopClosure, DerivedOopClosure, SkipNullValue> visitor(oop_fn, derived_oop_fn);
  visitor.oops_do(fr, reg_map, this);
}

template<typename T>
static void iterate_all_do(const frame *fr, int mask, OopMapClosure* fn, const ImmutableOopMap* oopmap) {
  OopMapValue omv;
  for (T oms(oopmap,mask); !oms.is_done(); oms.next()) {
      omv = oms.current();
      fn->do_value(omv.reg(), omv.type());
  }
}

void ImmutableOopMap::all_do(const frame *fr, int mask, OopMapClosure* fn) const {
  if (_exploded != NULL) {
    iterate_all_do<ExplodedOopMapStream>(fr, mask, fn, this);
  } else {
    iterate_all_do<OopMapStream>(fr, mask, fn, this);
  }
}

template <typename T>
static void update_register_map1(const ImmutableOopMap* oopmap, const frame* fr, RegisterMap* reg_map) {
  for (T oms(oopmap, OopMapValue::callee_saved_value); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    VMReg reg = omv.content_reg();
    oop* loc = fr->oopmapreg_to_location(omv.reg(), reg_map);
    reg_map->set_location(reg, (address) loc);
    //DEBUG_ONLY(nof_callee++;)
  }
}

void ImmutableOopMap::update_register_map(const frame *fr, RegisterMap *reg_map) const {
  // ResourceMark rm;
  CodeBlob* cb = fr->cb();
  assert(cb != NULL, "no codeblob");
  // Any reg might be saved by a safepoint handler (see generate_handler_blob).
  assert( reg_map->_update_for_id == NULL || fr->is_older(reg_map->_update_for_id),
         "already updated this map; do not 'update' it twice!" );
  debug_only(reg_map->_update_for_id = fr->id());


  // Check if caller must update oop argument
  assert((reg_map->include_argument_oops() ||
          !cb->caller_must_gc_arguments(reg_map->thread())),
         "include_argument_oops should already be set");

  // Scan through oopmap and find location of all callee-saved registers
  // (we do not do update in place, since info could be overwritten)

  DEBUG_ONLY(int nof_callee = 0;)
  if (_exploded != NULL) {
    update_register_map1<ExplodedOopMapStream>(this, fr, reg_map);
  } else {
    update_register_map1<OopMapStream>(this, fr, reg_map);
  }

  /*
  for (OopMapStream oms(this, OopMapValue::callee_saved_value); !oms.is_done(); oms.next()) {
    OopMapValue omv = oms.current();
    VMReg reg = omv.content_reg();
    oop* loc = fr->oopmapreg_to_location(omv.reg(), reg_map);
    reg_map->set_location(reg, (address) loc);
    DEBUG_ONLY(nof_callee++;)
  }
  */

  // Check that runtime stubs save all callee-saved registers
#ifdef COMPILER2
  assert(cb == NULL || cb->is_compiled_by_c1() || cb->is_compiled_by_jvmci() || !cb->is_runtime_stub() ||
         (nof_callee >= SAVED_ON_ENTRY_REG_COUNT || nof_callee >= C_SAVED_ON_ENTRY_REG_COUNT),
         "must save all");
#endif // COMPILER2
}

const ImmutableOopMap* OopMapSet::find_map(const frame *fr) { 
  return find_map(fr->cb(), fr->pc()); 
}

const ImmutableOopMap* OopMapSet::find_map(const CodeBlob* cb, address pc) {
  assert(cb != NULL, "no codeblob");
  const ImmutableOopMap* map = cb->oop_map_for_return_address(pc);
  assert(map != NULL, "no ptr map found");
  return map;
}

// Update callee-saved register info for the following frame
void OopMapSet::update_register_map(const frame *fr, RegisterMap *reg_map) {
  find_map(fr)->update_register_map(fr, reg_map);
}

//=============================================================================
// Non-Product code

#ifndef PRODUCT

bool ImmutableOopMap::has_derived_pointer() const {
#if !defined(TIERED) && !INCLUDE_JVMCI
  COMPILER1_PRESENT(return false);
#endif // !TIERED
#if COMPILER2_OR_JVMCI
  OopMapStream oms(this,OopMapValue::derived_oop_value);
  return oms.is_done();
#else
  return false;
#endif // COMPILER2_OR_JVMCI
}

#ifndef PRODUCT
void OopMapSet::trace_codeblob_maps(const frame *fr, const RegisterMap *reg_map) {
  // Print oopmap and regmap
  tty->print_cr("------ ");
  CodeBlob* cb = fr->cb();
  const ImmutableOopMapSet* maps = cb->oop_maps();
  const ImmutableOopMap* map = cb->oop_map_for_return_address(fr->pc());
  map->print();
  if( cb->is_nmethod() ) {
    nmethod* nm = (nmethod*)cb;
    // native wrappers have no scope data, it is implied
    if (nm->is_native_method()) {
      tty->print("bci: 0 (native)");
    } else {
      ScopeDesc* scope  = nm->scope_desc_at(fr->pc());
      tty->print("bci: %d ",scope->bci());
    }
  }
  tty->cr();
  fr->print_on(tty);
  tty->print("     ");
  cb->print_value_on(tty);  tty->cr();
  reg_map->print();
  tty->print_cr("------ ");

}
#endif // PRODUCT


#endif //PRODUCT

// Printing code is present in product build for -XX:+PrintAssembly.

static
void print_register_type(OopMapValue::oop_types x, VMReg optional,
                         outputStream* st) {
  switch( x ) {
  case OopMapValue::oop_value:
    st->print("Oop");
    break;
  case OopMapValue::narrowoop_value:
    st->print("NarrowOop");
    break;
  case OopMapValue::callee_saved_value:
    st->print("Callers_");
    optional->print_on(st);
    break;
  case OopMapValue::derived_oop_value:
    st->print("Derived_oop_");
    optional->print_on(st);
    break;
  default:
    ShouldNotReachHere();
  }
}

void OopMapValue::print_on(outputStream* st) const {
  reg()->print_on(st);
  st->print("=");
  print_register_type(type(),content_reg(),st);
  st->print(" ");
}

void ImmutableOopMap::print_on(outputStream* st) const {
  OopMapValue omv;
  st->print("ImmutableOopMap{");
  for(OopMapStream oms(this); !oms.is_done(); oms.next()) {
    omv = oms.current();
    omv.print_on(st);
  }
  st->print("}");
}

void OopMap::print_on(outputStream* st) const {
  OopMapValue omv;
  st->print("OopMap{");
  for(OopMapStream oms((OopMap*)this); !oms.is_done(); oms.next()) {
    omv = oms.current();
    omv.print_on(st);
  }
  st->print("off=%d}", (int) offset());
}

void ImmutableOopMapSet::print_on(outputStream* st) const {
  const ImmutableOopMap* last = NULL;
  for (int i = 0; i < _count; ++i) {
    const ImmutableOopMapPair* pair = pair_at(i);
    const ImmutableOopMap* map = pair->get_from(this);
    if (map != last) {
      st->cr();
      map->print_on(st);
      st->print("pc offsets: ");
    }
    last = map;
    st->print("%d ", pair->pc_offset());
  }
}

void OopMapSet::print_on(outputStream* st) const {
  int i, len = om_count();

  st->print_cr("OopMapSet contains %d OopMaps\n",len);

  for( i = 0; i < len; i++) {
    OopMap* m = at(i);
    st->print_cr("#%d ",i);
    m->print_on(st);
    st->cr();
  }
}

bool OopMap::equals(const OopMap* other) const {
  if (other->_omv_count != _omv_count) {
    return false;
  }
  if (other->write_stream()->position() != write_stream()->position()) {
    return false;
  }
  if (memcmp(other->write_stream()->buffer(), write_stream()->buffer(), write_stream()->position()) != 0) {
    return false;
  }
  return true;
}

int ImmutableOopMapSet::find_slot_for_offset(int pc_offset) const {
  ImmutableOopMapPair* pairs = get_pairs();

  int i;
  for (i = 0; i < _count; ++i) {
    if (pairs[i].pc_offset() >= pc_offset) {
      break;
    }
  }
  ImmutableOopMapPair* last = &pairs[i];
  assert(last->pc_offset() == pc_offset, "oopmap not found");
  return i;
}

const ImmutableOopMap* ImmutableOopMapSet::find_map_at_offset(int pc_offset) const {
  ImmutableOopMapPair* pairs = get_pairs();

  int i;
  for (i = 0; i < _count; ++i) {
    if (pairs[i].pc_offset() >= pc_offset) {
      break;
    }
  }
  ImmutableOopMapPair* last = &pairs[i];

  assert(last->pc_offset() == pc_offset, "oopmap not found");
  return last->get_from(this);
}

ImmutableOopMap::ImmutableOopMap(const OopMap* oopmap) : _exploded(NULL), _freeze_stub(NULL), _thaw_stub(NULL), _count(oopmap->count()), _num_oops(oopmap->num_oops()) {
  _num_oops = oopmap->num_oops();
  address addr = data_addr();
  //oopmap->copy_data_to(addr);
  oopmap->copy_and_sort_data_to(addr);
}

#ifdef ASSERT
int ImmutableOopMap::nr_of_bytes() const {
  OopMapStream oms(this);

  while (!oms.is_done()) {
    oms.next();
  }
  return sizeof(ImmutableOopMap) + oms.stream_position();
}
#endif

ImmutableOopMapBuilder::ImmutableOopMapBuilder(const OopMapSet* set)
  : _set(set), _empty(NULL), _last(NULL), _empty_offset(-1), _last_offset(-1), _offset(0), _required(-1), _new_set(NULL) {
  _mapping = NEW_RESOURCE_ARRAY(Mapping, _set->size());
}

int ImmutableOopMapBuilder::size_for(const OopMap* map) const {
  return align_up((int)sizeof(ImmutableOopMap) + map->data_size(), 8);
}

int ImmutableOopMapBuilder::heap_size() {
  int base = sizeof(ImmutableOopMapSet);
  base = align_up(base, 8);

  // all of ours pc / offset pairs
  int pairs = _set->size() * sizeof(ImmutableOopMapPair);
  pairs = align_up(pairs, 8);

  for (int i = 0; i < _set->size(); ++i) {
    int size = 0;
    OopMap* map = _set->at(i);

    if (is_empty(map)) {
      /* only keep a single empty map in the set */
      if (has_empty()) {
        _mapping[i].set(Mapping::OOPMAP_EMPTY, _empty_offset, 0, map, _empty);
      } else {
        _empty_offset = _offset;
        _empty = map;
        size = size_for(map);
        _mapping[i].set(Mapping::OOPMAP_NEW, _offset, size, map);
      }
    } else if (is_last_duplicate(map)) {
      /* if this entry is identical to the previous one, just point it there */
      _mapping[i].set(Mapping::OOPMAP_DUPLICATE, _last_offset, 0, map, _last);
    } else {
      /* not empty, not an identical copy of the previous entry */
      size = size_for(map);
      _mapping[i].set(Mapping::OOPMAP_NEW, _offset, size, map);
      _last_offset = _offset;
      _last = map;
    }

    assert(_mapping[i]._map == map, "check");
    _offset += size;
  }

  int total = base + pairs + _offset;
  DEBUG_ONLY(total += 8);
  _required = total;
  return total;
}

void ImmutableOopMapBuilder::fill_pair(ImmutableOopMapPair* pair, const OopMap* map, int offset, const ImmutableOopMapSet* set) {
  assert(offset < set->nr_of_bytes(), "check");
  new ((address) pair) ImmutableOopMapPair(map->offset(), offset);
}

int ImmutableOopMapBuilder::fill_map(ImmutableOopMapPair* pair, const OopMap* map, int offset, const ImmutableOopMapSet* set) {
  fill_pair(pair, map, offset, set);
  address addr = (address) pair->get_from(_new_set); // location of the ImmutableOopMap

  new (addr) ImmutableOopMap(map);
  return size_for(map);
}

void ImmutableOopMapBuilder::fill(ImmutableOopMapSet* set, int sz) {
  ImmutableOopMapPair* pairs = set->get_pairs();

  for (int i = 0; i < set->count(); ++i) {
    const OopMap* map = _mapping[i]._map;
    ImmutableOopMapPair* pair = NULL;
    int size = 0;

    if (_mapping[i]._kind == Mapping::OOPMAP_NEW) {
      size = fill_map(&pairs[i], map, _mapping[i]._offset, set);
    } else if (_mapping[i]._kind == Mapping::OOPMAP_DUPLICATE || _mapping[i]._kind == Mapping::OOPMAP_EMPTY) {
      fill_pair(&pairs[i], map, _mapping[i]._offset, set);
    }

    const ImmutableOopMap* nv = set->find_map_at_offset(map->offset());
    //assert(memcmp(map->data(), nv->data_addr(), map->data_size()) == 0, "check identity");
  }
}

#ifdef ASSERT
void ImmutableOopMapBuilder::verify(address buffer, int size, const ImmutableOopMapSet* set) {
  for (int i = 0; i < 8; ++i) {
    assert(buffer[size - 8 + i] == (unsigned char) 0xff, "overwritten memory check");
  }

  for (int i = 0; i < set->count(); ++i) {
    const ImmutableOopMapPair* pair = set->pair_at(i);
    assert(pair->oopmap_offset() < set->nr_of_bytes(), "check size");
    const ImmutableOopMap* map = pair->get_from(set);
    int nr_of_bytes = map->nr_of_bytes();
    assert(pair->oopmap_offset() + nr_of_bytes <= set->nr_of_bytes(), "check size + size");
  }
}
#endif

ImmutableOopMapSet* ImmutableOopMapBuilder::generate_into(address buffer) {
  DEBUG_ONLY(memset(&buffer[_required-8], 0xff, 8));

  _new_set = new (buffer) ImmutableOopMapSet(_set, _required);
  fill(_new_set, _required);

  DEBUG_ONLY(verify(buffer, _required, _new_set));

  return _new_set;
}

ImmutableOopMapSet* ImmutableOopMapBuilder::build() {
  _required = heap_size();

  // We need to allocate a chunk big enough to hold the ImmutableOopMapSet and all of its ImmutableOopMaps
  address buffer = (address) NEW_C_HEAP_ARRAY(unsigned char, _required, mtCode);
  return generate_into(buffer);
}

ImmutableOopMapSet* ImmutableOopMapSet::build_from(const OopMapSet* oopmap_set) {
  ResourceMark mark;
  ImmutableOopMapBuilder builder(oopmap_set);
  return builder.build();
}


//------------------------------DerivedPointerTable---------------------------

#if COMPILER2_OR_JVMCI

class DerivedPointerEntry : public CHeapObj<mtCompiler> {
 private:
  oop*     _location; // Location of derived pointer (also pointing to the base)
  intptr_t _offset;   // Offset from base pointer
 public:
  DerivedPointerEntry(oop* location, intptr_t offset) { _location = location; _offset = offset; }
  oop* location()    { return _location; }
  intptr_t  offset() { return _offset; }
};


GrowableArray<DerivedPointerEntry*>* DerivedPointerTable::_list = NULL;
bool DerivedPointerTable::_active = false;


void DerivedPointerTable::clear() {
  // The first time, we create the list.  Otherwise it should be
  // empty.  If not, then we have probably forgotton to call
  // update_pointers after last GC/Scavenge.
  assert (!_active, "should not be active");
  assert(_list == NULL || _list->length() == 0, "table not empty");
  if (_list == NULL) {
    _list = new (ResourceObj::C_HEAP, mtCompiler) GrowableArray<DerivedPointerEntry*>(10, true); // Allocated on C heap
  }
  _active = true;
}


// Returns value of location as an int
intptr_t value_of_loc(oop *pointer) { return cast_from_oop<intptr_t>((*pointer)); }


void DerivedPointerTable::add(oop *derived_loc, oop *base_loc) {
  assert(Universe::heap()->is_in_or_null(*base_loc), "not an oop");
  assert(derived_loc != base_loc, "Base and derived in same location");
  if (_active) {
    assert(*derived_loc != (oop)base_loc, "location already added");
    assert(_list != NULL, "list must exist");
    intptr_t offset = value_of_loc(derived_loc) - value_of_loc(base_loc);
    // This assert is invalid because derived pointers can be
    // arbitrarily far away from their base.
    // assert(offset >= -1000000, "wrong derived pointer info");

    if (TraceDerivedPointers) {
      tty->print_cr(
        "Add derived pointer@" INTPTR_FORMAT
        " - Derived: " INTPTR_FORMAT
        " Base: " INTPTR_FORMAT " (@" INTPTR_FORMAT ") (Offset: " INTX_FORMAT ")",
        p2i(derived_loc), p2i((address)*derived_loc), p2i((address)*base_loc), p2i(base_loc), offset
      );
    }
    // Set derived oop location to point to base.
    *derived_loc = (oop)base_loc;
    assert_lock_strong(DerivedPointerTableGC_lock);
    DerivedPointerEntry *entry = new DerivedPointerEntry(derived_loc, offset);
    _list->append(entry);
  }
}


void DerivedPointerTable::update_pointers() {
  assert(_list != NULL, "list must exist");
  for(int i = 0; i < _list->length(); i++) {
    DerivedPointerEntry* entry = _list->at(i);
    oop* derived_loc = entry->location();
    intptr_t offset  = entry->offset();
    // The derived oop was setup to point to location of base
    oop  base        = **(oop**)derived_loc;
    assert(Universe::heap()->is_in_or_null(base), "must be an oop");

    *derived_loc = (oop)(((address)base) + offset);
    assert(value_of_loc(derived_loc) - value_of_loc(&base) == offset, "sanity check");

    if (TraceDerivedPointers) {
      tty->print_cr("Updating derived pointer@" INTPTR_FORMAT
                    " - Derived: " INTPTR_FORMAT "  Base: " INTPTR_FORMAT " (Offset: " INTX_FORMAT ")",
          p2i(derived_loc), p2i((address)*derived_loc), p2i((address)base), offset);
    }

    // Delete entry
    delete entry;
    _list->at_put(i, NULL);
  }
  // Clear list, so it is ready for next traversal (this is an invariant)
  if (TraceDerivedPointers && !_list->is_empty()) {
    tty->print_cr("--------------------------");
  }
  _list->clear();
  _active = false;
}

#endif // COMPILER2_OR_JVMCI
