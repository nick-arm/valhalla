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
#include "ci/ciValueKlass.hpp"
#include "classfile/systemDictionary.hpp"
#include "compiler/compileLog.hpp"
#include "oops/flatArrayKlass.hpp"
#include "oops/objArrayKlass.hpp"
#include "opto/addnode.hpp"
#include "opto/castnode.hpp"
#include "opto/memnode.hpp"
#include "opto/mulnode.hpp"
#include "opto/parse.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/valuetypenode.hpp"
#include "runtime/sharedRuntime.hpp"

//------------------------------make_dtrace_method_entry_exit ----------------
// Dtrace -- record entry or exit of a method if compiled with dtrace support
void GraphKit::make_dtrace_method_entry_exit(ciMethod* method, bool is_entry) {
  const TypeFunc *call_type    = OptoRuntime::dtrace_method_entry_exit_Type();
  address         call_address = is_entry ? CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry) :
                                            CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit);
  const char     *call_name    = is_entry ? "dtrace_method_entry" : "dtrace_method_exit";

  // Get base of thread-local storage area
  Node* thread = _gvn.transform( new ThreadLocalNode() );

  // Get method
  const TypePtr* method_type = TypeMetadataPtr::make(method);
  Node *method_node = _gvn.transform(ConNode::make(method_type));

  kill_dead_locals();

  // For some reason, this call reads only raw memory.
  const TypePtr* raw_adr_type = TypeRawPtr::BOTTOM;
  make_runtime_call(RC_LEAF | RC_NARROW_MEM,
                    call_type, call_address,
                    call_name, raw_adr_type,
                    thread, method_node);
}


//=============================================================================
//------------------------------do_checkcast-----------------------------------
void Parse::do_checkcast() {
  bool will_link;
  ciKlass* klass = iter().get_klass(will_link);
  bool never_null = iter().is_klass_never_null();

  Node *obj = peek();

  // Throw uncommon trap if class is not loaded or the value we are casting
  // _from_ is not loaded, and value is not null.  If the value _is_ NULL,
  // then the checkcast does nothing.
  const TypeOopPtr *tp = _gvn.type(obj)->isa_oopptr();
  if (!will_link || (tp && tp->klass() && !tp->klass()->is_loaded())) {
    assert(!never_null, "Null-free value type should be loaded");
    if (C->log() != NULL) {
      if (!will_link) {
        C->log()->elem("assert_null reason='checkcast' klass='%d'",
                       C->log()->identify(klass));
      }
      if (tp && tp->klass() && !tp->klass()->is_loaded()) {
        // %%% Cannot happen?
        C->log()->elem("assert_null reason='checkcast source' klass='%d'",
                       C->log()->identify(tp->klass()));
      }
    }
    null_assert(obj);
    assert( stopped() || _gvn.type(peek())->higher_equal(TypePtr::NULL_PTR), "what's left behind is null" );
    if (!stopped()) {
      profile_null_checkcast();
    }
    return;
  }

  Node* res = gen_checkcast(obj, makecon(TypeKlassPtr::make(klass)), NULL, never_null);
  if (stopped()) {
    return;
  }

  // Pop from stack AFTER gen_checkcast because it can uncommon trap and
  // the debug info has to be correct.
  pop();
  push(res);
}


//------------------------------do_instanceof----------------------------------
void Parse::do_instanceof() {
  if (stopped())  return;
  // We would like to return false if class is not loaded, emitting a
  // dependency, but Java requires instanceof to load its operand.

  // Throw uncommon trap if class is not loaded
  bool will_link;
  ciKlass* klass = iter().get_klass(will_link);

  if (!will_link) {
    if (C->log() != NULL) {
      C->log()->elem("assert_null reason='instanceof' klass='%d'",
                     C->log()->identify(klass));
    }
    null_assert(peek());
    assert( stopped() || _gvn.type(peek())->higher_equal(TypePtr::NULL_PTR), "what's left behind is null" );
    if (!stopped()) {
      // The object is now known to be null.
      // Shortcut the effect of gen_instanceof and return "false" directly.
      pop();                   // pop the null
      push(_gvn.intcon(0));    // push false answer
    }
    return;
  }

  // Push the bool result back on stack
  Node* res = gen_instanceof(peek(), makecon(TypeKlassPtr::make(klass)), true);

  // Pop from stack AFTER gen_instanceof because it can uncommon trap.
  pop();
  push(res);
}

//------------------------------array_store_check------------------------------
// pull array from stack and check that the store is valid
Node* Parse::array_store_check() {
  // Shorthand access to array store elements without popping them.
  Node *obj = peek(0);
  Node *idx = peek(1);
  Node *ary = peek(2);

  if (_gvn.type(obj) == TypePtr::NULL_PTR) {
    // There's never a type check on null values.
    // This cutout lets us avoid the uncommon_trap(Reason_array_check)
    // below, which turns into a performance liability if the
    // gen_checkcast folds up completely.
    return obj;
  }

  // Extract the array klass type
  Node* array_klass = load_object_klass(ary);
  // Get the array klass
  const TypeKlassPtr *tak = _gvn.type(array_klass)->is_klassptr();

  // The type of array_klass is usually INexact array-of-oop.  Heroically
  // cast array_klass to EXACT array and uncommon-trap if the cast fails.
  // Make constant out of the inexact array klass, but use it only if the cast
  // succeeds.
  bool always_see_exact_class = false;
  if (MonomorphicArrayCheck
      && !tak->klass_is_exact()) {
      // Regarding the fourth condition in the if-statement from above:
      //
      // If the compiler has determined that the type of array 'ary' (represented
      // by 'array_klass') is java/lang/Object, the compiler must not assume that
      // the array 'ary' is monomorphic.
      //
      // If 'ary' were of type java/lang/Object, this arraystore would have to fail,
      // because it is not possible to perform a arraystore into an object that is not
      // a "proper" array.
      //
      // Therefore, let's obtain at runtime the type of 'ary' and check if we can still
      // successfully perform the store.
      //
      // The implementation reasons for the condition are the following:
      //
      // java/lang/Object is the superclass of all arrays, but it is represented by the VM
      // as an InstanceKlass. The checks generated by gen_checkcast() (see below) expect
      // 'array_klass' to be ObjArrayKlass, which can result in invalid memory accesses.
      //
      // See issue JDK-8057622 for details.

    // (If no MDO at all, hope for the best, until a trap actually occurs.)

    // Make a constant out of the inexact array klass
    const TypeKlassPtr *extak = NULL;
    const TypeOopPtr* ary_t = _gvn.type(ary)->is_oopptr();
    ciKlass* ary_spec = ary_t->speculative_type();
    Deoptimization::DeoptReason reason = Deoptimization::Reason_none;
    // Try to cast the array to an exact type from profile data. First
    // check the speculative type.
    if (ary_spec != NULL && !too_many_traps(Deoptimization::Reason_speculate_class_check)) {
      extak = TypeKlassPtr::make(ary_spec);
      reason = Deoptimization::Reason_speculate_class_check;
    } else if (UseArrayLoadStoreProfile) {
      // No speculative type: check profile data at this bci.
      reason = Deoptimization::Reason_class_check;
      if (!too_many_traps(reason)) {
        ciKlass* array_type = NULL;
        ciKlass* element_type = NULL;
        ProfilePtrKind element_ptr = ProfileMaybeNull;
        bool flat_array = true;
        bool null_free_array = true;
        method()->array_access_profiled_type(bci(), array_type, element_type, element_ptr, flat_array, null_free_array);
        if (array_type != NULL) {
          extak = TypeKlassPtr::make(array_type);
        }
      }
    } else if (!too_many_traps(Deoptimization::Reason_array_check) && tak != TypeKlassPtr::OBJECT) {
      extak = tak->cast_to_exactness(true)->is_klassptr();
      reason = Deoptimization::Reason_array_check;
    }
    if (extak != NULL) {
      always_see_exact_class = true;
      Node* con = makecon(extak);
      Node* cmp = _gvn.transform(new CmpPNode( array_klass, con ));
      Node* bol = _gvn.transform(new BoolNode( cmp, BoolTest::eq ));
      Node* ctrl= control();
      { BuildCutout unless(this, bol, PROB_MAX);
        uncommon_trap(reason,
                      Deoptimization::Action_maybe_recompile,
                      tak->klass());
      }
      if (stopped()) {          // MUST uncommon-trap?
        set_control(ctrl);      // Then Don't Do It, just fall into the normal checking
      } else {                  // Cast array klass to exactness:
        // Use the exact constant value we know it is.
        replace_in_map(array_klass,con);
        Node* cast = _gvn.transform(new CheckCastPPNode(control(), ary, extak->as_instance_type()));
        replace_in_map(ary, cast);

        CompileLog* log = C->log();
        if (log != NULL) {
          log->elem("cast_up reason='monomorphic_array' from='%d' to='(exact)'",
                    log->identify(tak->klass()));
        }
        array_klass = con;      // Use cast value moving forward
      }
    }
  }

  // Come here for polymorphic array klasses

  // Extract the array element class
  int element_klass_offset = in_bytes(ArrayKlass::element_klass_offset());

  Node *p2 = basic_plus_adr(array_klass, array_klass, element_klass_offset);
  // We are allowed to use the constant type only if cast succeeded. If always_see_exact_class is true,
  // we must set a control edge from the IfTrue node created by the uncommon_trap above to the
  // LoadKlassNode.
  Node* a_e_klass = _gvn.transform(LoadKlassNode::make(_gvn, always_see_exact_class ? control() : NULL,
                                                       immutable_memory(), p2, tak));

  // Handle value type arrays
  const Type* elemtype = _gvn.type(ary)->is_aryptr()->elem();
  if (elemtype->isa_valuetype() != NULL || elemtype->is_valuetypeptr()) {
    // We statically know that this is a value type array, use precise klass ptr
    a_e_klass = makecon(TypeKlassPtr::make(elemtype->value_klass()));
  }

  // Check (the hard way) and throw if not a subklass.
  return gen_checkcast(obj, a_e_klass);
}


//------------------------------do_new-----------------------------------------
void Parse::do_new() {
  kill_dead_locals();

  bool will_link;
  ciInstanceKlass* klass = iter().get_klass(will_link)->as_instance_klass();
  assert(will_link, "_new: typeflow responsibility");

  // Should throw an InstantiationError?
  if (klass->is_abstract() || klass->is_interface() ||
      klass->name() == ciSymbol::java_lang_Class() ||
      iter().is_unresolved_klass()) {
    uncommon_trap(Deoptimization::Reason_unhandled,
                  Deoptimization::Action_none,
                  klass);
    return;
  }

  if (C->needs_clinit_barrier(klass, method())) {
    clinit_barrier(klass, method());
    if (stopped())  return;
  }

  Node* kls = makecon(TypeKlassPtr::make(klass));
  Node* obj = new_instance(kls);

  // Push resultant oop onto stack
  push(obj);

  // Keep track of whether opportunities exist for StringBuilder
  // optimizations.
  if (OptimizeStringConcat &&
      (klass == C->env()->StringBuilder_klass() ||
       klass == C->env()->StringBuffer_klass())) {
    C->set_has_stringbuilder(true);
  }

  // Keep track of boxed values for EliminateAutoBox optimizations.
  if (C->eliminate_boxing() && klass->is_box_klass()) {
    C->set_has_boxed_value(true);
  }
}

//------------------------------do_defaultvalue---------------------------------
void Parse::do_defaultvalue() {
  bool will_link;
  ciValueKlass* vk = iter().get_klass(will_link)->as_value_klass();
  assert(will_link, "defaultvalue: typeflow responsibility");

  // Should throw an InstantiationError?
  if (iter().is_unresolved_klass()) {
    uncommon_trap(Deoptimization::Reason_unhandled,
                  Deoptimization::Action_none,
                  vk);
    return;
  }

  if (C->needs_clinit_barrier(vk, method())) {
    clinit_barrier(vk, method());
    if (stopped())  return;
  }

  ValueTypeNode* vt = ValueTypeNode::make_default(_gvn, vk);
  if (vk->is_scalarizable()) {
    push(vt);
  } else {
    push(vt->get_oop());
  }
}

//------------------------------do_withfield------------------------------------
void Parse::do_withfield() {
  bool will_link;
  ciField* field = iter().get_field(will_link);
  assert(will_link, "withfield: typeflow responsibility");
  Node* val = pop_node(field->layout_type());
  ciValueKlass* holder_klass = field->holder()->as_value_klass();
  Node* holder = pop();
  int nargs = 1 + field->type()->size();

  if (!holder->is_ValueType()) {
    // Null check and scalarize value type holder
    inc_sp(nargs);
    holder = null_check(holder);
    dec_sp(nargs);
    if (stopped()) return;
    holder = ValueTypeNode::make_from_oop(this, holder, holder_klass);
  }
  if (!val->is_ValueType() && field->is_flattenable()) {
    // Null check and scalarize value type field value
    inc_sp(nargs);
    val = null_check(val);
    dec_sp(nargs);
    if (stopped()) return;
    val = ValueTypeNode::make_from_oop(this, val, gvn().type(val)->value_klass());
  } else if (val->is_ValueType() && !field->is_flattenable()) {
    // Non-flattenable field value needs to be allocated because it can be merged
    // with an oop. Re-execute withfield if buffering triggers deoptimization.
    PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);
    inc_sp(nargs);
    val = val->as_ValueType()->buffer(this);
  }

  // Clone the value type node and set the new field value
  ValueTypeNode* new_vt = holder->clone()->as_ValueType();
  new_vt->set_oop(_gvn.zerocon(T_INLINE_TYPE));
  gvn().set_type(new_vt, new_vt->bottom_type());
  new_vt->set_field_value_by_offset(field->offset(), val);
  Node* res = new_vt;

  if (!holder_klass->is_scalarizable()) {
    // Re-execute withfield if buffering triggers deoptimization
    PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);
    inc_sp(nargs);
    res = new_vt->buffer(this)->get_oop();
  }
  push(_gvn.transform(res));
}

#ifndef PRODUCT
//------------------------------dump_map_adr_mem-------------------------------
// Debug dump of the mapping from address types to MergeMemNode indices.
void Parse::dump_map_adr_mem() const {
  tty->print_cr("--- Mapping from address types to memory Nodes ---");
  MergeMemNode *mem = map() == NULL ? NULL : (map()->memory()->is_MergeMem() ?
                                      map()->memory()->as_MergeMem() : NULL);
  for (uint i = 0; i < (uint)C->num_alias_types(); i++) {
    C->alias_type(i)->print_on(tty);
    tty->print("\t");
    // Node mapping, if any
    if (mem && i < mem->req() && mem->in(i) && mem->in(i) != mem->empty_memory()) {
      mem->in(i)->dump();
    } else {
      tty->cr();
    }
  }
}

#endif


//=============================================================================
//
// parser methods for profiling


//----------------------test_counter_against_threshold ------------------------
void Parse::test_counter_against_threshold(Node* cnt, int limit) {
  // Test the counter against the limit and uncommon trap if greater.

  // This code is largely copied from the range check code in
  // array_addressing()

  // Test invocation count vs threshold
  Node *threshold = makecon(TypeInt::make(limit));
  Node *chk   = _gvn.transform( new CmpUNode( cnt, threshold) );
  BoolTest::mask btest = BoolTest::lt;
  Node *tst   = _gvn.transform( new BoolNode( chk, btest) );
  // Branch to failure if threshold exceeded
  { BuildCutout unless(this, tst, PROB_ALWAYS);
    uncommon_trap(Deoptimization::Reason_age,
                  Deoptimization::Action_maybe_recompile);
  }
}

//----------------------increment_and_test_invocation_counter-------------------
void Parse::increment_and_test_invocation_counter(int limit) {
  if (!count_invocations()) return;

  // Get the Method* node.
  ciMethod* m = method();
  MethodCounters* counters_adr = m->ensure_method_counters();
  if (counters_adr == NULL) {
    C->record_failure("method counters allocation failed");
    return;
  }

  Node* ctrl = control();
  const TypePtr* adr_type = TypeRawPtr::make((address) counters_adr);
  Node *counters_node = makecon(adr_type);
  Node* adr_iic_node = basic_plus_adr(counters_node, counters_node,
    MethodCounters::interpreter_invocation_counter_offset_in_bytes());
  Node* cnt = make_load(ctrl, adr_iic_node, TypeInt::INT, T_INT, adr_type, MemNode::unordered);

  test_counter_against_threshold(cnt, limit);

  // Add one to the counter and store
  Node* incr = _gvn.transform(new AddINode(cnt, _gvn.intcon(1)));
  store_to_memory(ctrl, adr_iic_node, incr, T_INT, adr_type, MemNode::unordered);
}

//----------------------------method_data_addressing---------------------------
Node* Parse::method_data_addressing(ciMethodData* md, ciProfileData* data, ByteSize counter_offset, Node* idx, uint stride) {
  // Get offset within MethodData* of the data array
  ByteSize data_offset = MethodData::data_offset();

  // Get cell offset of the ProfileData within data array
  int cell_offset = md->dp_to_di(data->dp());

  // Add in counter_offset, the # of bytes into the ProfileData of counter or flag
  int offset = in_bytes(data_offset) + cell_offset + in_bytes(counter_offset);

  const TypePtr* adr_type = TypeMetadataPtr::make(md);
  Node* mdo = makecon(adr_type);
  Node* ptr = basic_plus_adr(mdo, mdo, offset);

  if (stride != 0) {
    Node* str = _gvn.MakeConX(stride);
    Node* scale = _gvn.transform( new MulXNode( idx, str ) );
    ptr   = _gvn.transform( new AddPNode( mdo, ptr, scale ) );
  }

  return ptr;
}

//--------------------------increment_md_counter_at----------------------------
void Parse::increment_md_counter_at(ciMethodData* md, ciProfileData* data, ByteSize counter_offset, Node* idx, uint stride) {
  Node* adr_node = method_data_addressing(md, data, counter_offset, idx, stride);

  const TypePtr* adr_type = _gvn.type(adr_node)->is_ptr();
  Node* cnt  = make_load(NULL, adr_node, TypeInt::INT, T_INT, adr_type, MemNode::unordered);
  Node* incr = _gvn.transform(new AddINode(cnt, _gvn.intcon(DataLayout::counter_increment)));
  store_to_memory(NULL, adr_node, incr, T_INT, adr_type, MemNode::unordered);
}

//--------------------------test_for_osr_md_counter_at-------------------------
void Parse::test_for_osr_md_counter_at(ciMethodData* md, ciProfileData* data, ByteSize counter_offset, int limit) {
  Node* adr_node = method_data_addressing(md, data, counter_offset);

  const TypePtr* adr_type = _gvn.type(adr_node)->is_ptr();
  Node* cnt  = make_load(NULL, adr_node, TypeInt::INT, T_INT, adr_type, MemNode::unordered);

  test_counter_against_threshold(cnt, limit);
}

//-------------------------------set_md_flag_at--------------------------------
void Parse::set_md_flag_at(ciMethodData* md, ciProfileData* data, int flag_constant) {
  Node* adr_node = method_data_addressing(md, data, DataLayout::flags_offset());

  const TypePtr* adr_type = _gvn.type(adr_node)->is_ptr();
  Node* flags = make_load(NULL, adr_node, TypeInt::INT, T_INT, adr_type, MemNode::unordered);
  Node* incr = _gvn.transform(new OrINode(flags, _gvn.intcon(flag_constant)));
  store_to_memory(NULL, adr_node, incr, T_INT, adr_type, MemNode::unordered);
}

//----------------------------profile_taken_branch-----------------------------
void Parse::profile_taken_branch(int target_bci, bool force_update) {
  // This is a potential osr_site if we have a backedge.
  int cur_bci = bci();
  bool osr_site =
    (target_bci <= cur_bci) && count_invocations() && UseOnStackReplacement;

  // If we are going to OSR, restart at the target bytecode.
  set_bci(target_bci);

  // To do: factor out the the limit calculations below. These duplicate
  // the similar limit calculations in the interpreter.

  if (method_data_update() || force_update) {
    ciMethodData* md = method()->method_data();
    assert(md != NULL, "expected valid ciMethodData");
    ciProfileData* data = md->bci_to_data(cur_bci);
    assert(data != NULL && data->is_JumpData(), "need JumpData for taken branch");
    increment_md_counter_at(md, data, JumpData::taken_offset());
  }

  // In the new tiered system this is all we need to do. In the old
  // (c2 based) tiered sytem we must do the code below.
#ifndef TIERED
  if (method_data_update()) {
    ciMethodData* md = method()->method_data();
    if (osr_site) {
      ciProfileData* data = md->bci_to_data(cur_bci);
      assert(data != NULL && data->is_JumpData(), "need JumpData for taken branch");
      int limit = (int)((int64_t)CompileThreshold
                   * (OnStackReplacePercentage - InterpreterProfilePercentage) / 100);
      test_for_osr_md_counter_at(md, data, JumpData::taken_offset(), limit);
    }
  } else {
    // With method data update off, use the invocation counter to trigger an
    // OSR compilation, as done in the interpreter.
    if (osr_site) {
      int limit = (int)((int64_t)CompileThreshold * OnStackReplacePercentage / 100);
      increment_and_test_invocation_counter(limit);
    }
  }
#endif // TIERED

  // Restore the original bytecode.
  set_bci(cur_bci);
}

//--------------------------profile_not_taken_branch---------------------------
void Parse::profile_not_taken_branch(bool force_update) {

  if (method_data_update() || force_update) {
    ciMethodData* md = method()->method_data();
    assert(md != NULL, "expected valid ciMethodData");
    ciProfileData* data = md->bci_to_data(bci());
    assert(data != NULL && data->is_BranchData(), "need BranchData for not taken branch");
    increment_md_counter_at(md, data, BranchData::not_taken_offset());
  }

}

//---------------------------------profile_call--------------------------------
void Parse::profile_call(Node* receiver) {
  if (!method_data_update()) return;

  switch (bc()) {
  case Bytecodes::_invokevirtual:
  case Bytecodes::_invokeinterface:
    profile_receiver_type(receiver);
    break;
  case Bytecodes::_invokestatic:
  case Bytecodes::_invokedynamic:
  case Bytecodes::_invokespecial:
    profile_generic_call();
    break;
  default: fatal("unexpected call bytecode");
  }
}

//------------------------------profile_generic_call---------------------------
void Parse::profile_generic_call() {
  assert(method_data_update(), "must be generating profile code");

  ciMethodData* md = method()->method_data();
  assert(md != NULL, "expected valid ciMethodData");
  ciProfileData* data = md->bci_to_data(bci());
  assert(data != NULL && data->is_CounterData(), "need CounterData for not taken branch");
  increment_md_counter_at(md, data, CounterData::count_offset());
}

//-----------------------------profile_receiver_type---------------------------
void Parse::profile_receiver_type(Node* receiver) {
  assert(method_data_update(), "must be generating profile code");

  ciMethodData* md = method()->method_data();
  assert(md != NULL, "expected valid ciMethodData");
  ciProfileData* data = md->bci_to_data(bci());
  assert(data != NULL && data->is_ReceiverTypeData(), "need ReceiverTypeData here");

  // Skip if we aren't tracking receivers
  if (TypeProfileWidth < 1) {
    increment_md_counter_at(md, data, CounterData::count_offset());
    return;
  }
  ciReceiverTypeData* rdata = (ciReceiverTypeData*)data->as_ReceiverTypeData();

  Node* method_data = method_data_addressing(md, rdata, in_ByteSize(0));

  // Using an adr_type of TypePtr::BOTTOM to work around anti-dep problems.
  // A better solution might be to use TypeRawPtr::BOTTOM with RC_NARROW_MEM.
  make_runtime_call(RC_LEAF, OptoRuntime::profile_receiver_type_Type(),
                    CAST_FROM_FN_PTR(address,
                                     OptoRuntime::profile_receiver_type_C),
                    "profile_receiver_type_C",
                    TypePtr::BOTTOM,
                    method_data, receiver);
}

//---------------------------------profile_ret---------------------------------
void Parse::profile_ret(int target_bci) {
  if (!method_data_update()) return;

  // Skip if we aren't tracking ret targets
  if (TypeProfileWidth < 1) return;

  ciMethodData* md = method()->method_data();
  assert(md != NULL, "expected valid ciMethodData");
  ciProfileData* data = md->bci_to_data(bci());
  assert(data != NULL && data->is_RetData(), "need RetData for ret");
  ciRetData* ret_data = (ciRetData*)data->as_RetData();

  // Look for the target_bci is already in the table
  uint row;
  bool table_full = true;
  for (row = 0; row < ret_data->row_limit(); row++) {
    int key = ret_data->bci(row);
    table_full &= (key != RetData::no_bci);
    if (key == target_bci) break;
  }

  if (row >= ret_data->row_limit()) {
    // The target_bci was not found in the table.
    if (!table_full) {
      // XXX: Make slow call to update RetData
    }
    return;
  }

  // the target_bci is already in the table
  increment_md_counter_at(md, data, RetData::bci_count_offset(row));
}

//--------------------------profile_null_checkcast----------------------------
void Parse::profile_null_checkcast() {
  // Set the null-seen flag, done in conjunction with the usual null check. We
  // never unset the flag, so this is a one-way switch.
  if (!method_data_update()) return;

  ciMethodData* md = method()->method_data();
  assert(md != NULL, "expected valid ciMethodData");
  ciProfileData* data = md->bci_to_data(bci());
  assert(data != NULL && data->is_BitData(), "need BitData for checkcast");
  set_md_flag_at(md, data, BitData::null_seen_byte_constant());
}

//-----------------------------profile_switch_case-----------------------------
void Parse::profile_switch_case(int table_index) {
  if (!method_data_update()) return;

  ciMethodData* md = method()->method_data();
  assert(md != NULL, "expected valid ciMethodData");

  ciProfileData* data = md->bci_to_data(bci());
  assert(data != NULL && data->is_MultiBranchData(), "need MultiBranchData for switch case");
  if (table_index >= 0) {
    increment_md_counter_at(md, data, MultiBranchData::case_count_offset(table_index));
  } else {
    increment_md_counter_at(md, data, MultiBranchData::default_count_offset());
  }
}
