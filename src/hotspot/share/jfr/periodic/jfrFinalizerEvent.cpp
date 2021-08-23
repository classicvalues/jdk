/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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
#if INCLUDE_MANAGEMENT
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "jfr/jfrEvents.hpp"
#include "jfr/jni/jfrJavaSupport.hpp"
#include "jfr/periodic/jfrFinalizerEvent.hpp"
#include "jfr/support/jfrSymbolTable.hpp"
#include "jfr/utilities/jfrTime.hpp"
#include "jfr/utilities/jfrTypes.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"
#include "services/finalizerTable.hpp"

static oop get_codesource(oop pd, Thread* thread) {
  assert(pd != NULL, "invariant");
  assert(thread != NULL, "invariant");
  JavaValue result(T_OBJECT);
  JfrJavaArguments args(&result);
  args.set_klass(pd->klass());
  args.set_name("codesource");
  args.set_signature("Ljava/security/CodeSource;");
  args.set_receiver(pd);
  JfrJavaSupport::get_field(&args, thread);
  return result.get_oop();
}

// Caller needs ResourceMark
static const char* get_locationNoFragString(oop codesource, Thread* thread) {
  assert(codesource != NULL, "invariant");
  assert(thread != NULL, "invariant");
  JavaValue result(T_OBJECT);
  JfrJavaArguments args(&result);
  args.set_klass(codesource->klass());
  args.set_name("locationNoFragString");
  args.set_signature("Ljava/lang/String;");
  args.set_receiver(codesource);
  JfrJavaSupport::get_field(&args, thread);
  const oop string_oop = result.get_oop();
  return string_oop != NULL ? JfrJavaSupport::c_str(string_oop, thread) : NULL;
}

// Caller needs ResourceMark
static const char* codesource(const InstanceKlass* ik, Thread* thread) {
  assert(ik != NULL, "invariant");
  assert(thread != NULL, "invariant");
  oop pd = java_lang_Class::protection_domain(ik->java_mirror());
  if (pd == NULL) {
    return NULL;
  }
  oop codesource = get_codesource(pd, thread);
  return codesource != NULL ? get_locationNoFragString(codesource, thread) : NULL;
}

static void send_event(const FinalizerEntry* fe, const InstanceKlass* ik, const JfrTicks& timestamp, Thread* thread) {
  assert(ik != NULL, "invariant");
  assert(ik->has_finalizer(), "invariant");
  assert(thread != NULL, "invariant");
  const char* const url = codesource(ik, thread);
  const traceid codesource_symbol_id = url != NULL ? JfrSymbolTable::add(url) : 0;
  EventFinalizer event(UNTIMED);
  event.set_endtime(timestamp);
  event.set_overridingClass(ik);
  event.set_codeSource(codesource_symbol_id);
  if (fe == NULL) {
    event.set_registered(0);
    event.set_enqueued(0);
    event.set_finalized(0);
  } else {
    assert(fe->klass() == ik, "invariant");
    event.set_registered(fe->registered());
    event.set_enqueued(fe->enqueued());
    event.set_finalized(fe->finalized());
  }
  event.commit();
}

void JfrFinalizerEvent::send_unload_event(const InstanceKlass* ik) {
  assert(ik != NULL, "invariant");
  assert(ik->has_finalizer(), "invariant");
  Thread* const thread = Thread::current();
  ResourceMark rm(thread);
  send_event(FinalizerTable::lookup(ik, thread), ik, JfrTicks::now(), thread);
}

// Finalizer events generated by the periodic task thread
// during the same pass will all have the same timestamp.

class FinalizerEventClosure : public FinalizerEntryClosure {
 private:
  Thread* _thread;
  const JfrTicks _timestamp;
 public:
  FinalizerEventClosure(Thread* thread) : _thread(thread), _timestamp(JfrTicks::now()) {}
  virtual bool do_entry(const FinalizerEntry* fe) {
    assert(fe != NULL, "invariant");
    send_event(fe, fe->klass(), _timestamp, _thread);
    return true;
  }
};

void JfrFinalizerEvent::generate_events() {
  Thread* const thread = Thread::current();
  ResourceMark rm(thread);
  FinalizerEventClosure fec(thread);
  MutexLocker lock(ClassLoaderDataGraph_lock);
  FinalizerTable::do_entries(&fec, thread);
}

#endif // INCLUDE_MANAGEMENT
