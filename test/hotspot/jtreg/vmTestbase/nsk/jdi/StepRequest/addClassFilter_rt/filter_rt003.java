/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
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
 */

package nsk.jdi.StepRequest.addClassFilter_rt;

import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

import com.sun.jdi.*;
import com.sun.jdi.event.*;
import com.sun.jdi.request.*;

import java.util.*;
import java.io.*;

/**
 * The test for the implementation of an object of the type
 * StepRequest.
 *
 * The test checks that results of the method
 * <code>com.sun.jdi.StepRequest.addClassFilter(ReferenceType)</code>
 * complies with its spec.
 *
 * The test checks up on the following assertion:
 *    Restricts the events generated by this request to be
 *    the preparation of the given reference type and any subtypes.
 * The cases to test include re-invocations of the method
 * addClassFilter() on the same StepRequest object.
 * There are two StepRequests to check as follows:
 * (1) For StepRequest2, both invocations are with different
 *     ReferenceTypes restricting one Step event to two classes.
 *     The test expects no Step event will be received.
 * (2) For StepRequest1, both invocations are with the same
 *     ReferenceType restricting one Step event to one class.
 *     The test expects this Step event will be received.
 *
 * The test works as follows.
 * - The debugger resumes the debuggee and waits for the BreakpointEvent.
 * - The debuggee creates two threads, thread1 and thread2;
 *   thread1 will invoke methods in filter_rt003aTestClass10 and filter_rt003aTestClass11, and
 *   thread2 in filter_rt003aTestClass20 and filter_rt003aTestClass21,
 *   and steps into methods are objects to test;
 *   and invokes the methodForCommunication to be suspended and
 *   to inform the debugger with the event.
 * - Upon getting the BreakpointEvent, the debugger
 *   - gets ReferenceTypes 1&2 for the Classes to filter,
 *   - sets up two StepRequests 1&2,
 *   - double restricts StepRequest1 to the RefTypes 1 and 1,
 *   - double restricts StepRequest2 to the RefTypes 1 and 2,
 *   - resumes debuggee's main thread, and
 *   - waits for the event.
 * - Upon getting the events, the debugger performs checks required.
 */

public class filter_rt003 extends TestDebuggerType1 {

    public static void main (String argv[]) {
        System.exit(run(argv, System.out) + Consts.JCK_STATUS_BASE);
    }

    public static int run (String argv[], PrintStream out) {
        debuggeeName = "nsk.jdi.StepRequest.addClassFilter_rt.filter_rt003a";
        return new filter_rt003().runThis(argv, out);
    }

    private String testedClassName11 =
        "nsk.jdi.StepRequest.addClassFilter_rt.filter_rt003aTestClass11";
    private String testedClassName21 =
        "nsk.jdi.StepRequest.addClassFilter_rt.filter_rt003aTestClass21";

    protected void testRun() {

        EventRequest  eventRequest1 = null;
        EventRequest  eventRequest2 = null;

        String        property1     = "StepRequest1";
        String        property2     = "StepRequest2";

        ReferenceType testClassReference11 = null;
        ReferenceType testClassReference21 = null;

        ThreadReference thread1     = null;
        String          threadName1 = "thread1";

        ThreadReference thread2     = null;
        String          threadName2 = "thread2";

        for (int i = 0; ; i++) {

            if (!shouldRunAfterBreakpoint()) {
                vm.resume();
                break;
            }

            display(":::::: case: # " + i);

            switch (i) {

                case 0:
                testClassReference11 = (ReferenceType)debuggee.classByName(testedClassName11);
                testClassReference21 = (ReferenceType)debuggee.classByName(testedClassName21);

                thread1 = debuggee.threadByName(threadName1);
                thread2 = debuggee.threadByName(threadName2);

                eventRequest1 = setting21StepRequest(thread1, testClassReference11,
                                             EventRequest.SUSPEND_ALL, property1);

                eventRequest2 = setting21StepRequest(thread2, testClassReference11,
                                             EventRequest.SUSPEND_ALL, property2);

                ((StepRequest) eventRequest1).addClassFilter(testClassReference11);
                ((StepRequest) eventRequest2).addClassFilter(testClassReference21);

                display("......waiting for StepEvent in expected thread");
                Event newEvent = eventHandler.waitForRequestedEvent(new EventRequest[]{eventRequest1, eventRequest2}, waitTime, false);

                if ( !(newEvent instanceof StepEvent)) {
                    setFailedStatus("ERROR: new event is not StepEvent");
                } else {

                    String property = (String) newEvent.request().getProperty("number");
                    display("       got new StepEvent with property 'number' == " + property);

                    if ( !property.equals(property1) ) {
                        setFailedStatus("ERROR: property is not : " + property1);
                    }

                    ReferenceType refType = ((StepEvent)newEvent).location().declaringType();
                    if (!refType.equals(testClassReference11)) {
                        setFailedStatus("Received unexpected declaring type of the event: " + refType.name() +
                            "\n\texpected one: " + testClassReference11.name());
                    }
                }
                vm.resume();
                break;

                default:
                throw new Failure("** default case 2 **");
            }
        }
        return;
    }

    private StepRequest setting21StepRequest ( ThreadReference thread,
                                               ReferenceType   testedClass,
                                               int             suspendPolicy,
                                               String          property        ) {
        try {
            display("......setting up StepRequest:");
            display("       thread: " + thread + "; property: " + property);

            StepRequest
            str = eventRManager.createStepRequest(thread, StepRequest.STEP_LINE, StepRequest.STEP_INTO);
            str.putProperty("number", property);
            str.setSuspendPolicy(suspendPolicy);
            str.addClassFilter(testedClass);
            str.addCountFilter(1);

            display("      StepRequest has been set up");
            return str;
        } catch ( Exception e ) {
            throw new Failure("** FAILURE to set up StepRequest **");
        }
    }
}
