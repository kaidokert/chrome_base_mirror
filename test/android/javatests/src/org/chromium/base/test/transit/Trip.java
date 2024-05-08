// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base.test.transit;

import androidx.annotation.Nullable;

import org.chromium.base.Log;
import org.chromium.base.test.transit.ConditionWaiter.ConditionWait;
import org.chromium.base.test.transit.ConditionalState.Phase;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * A {@link Transition} into a {@link Station}, either from another {@link Station} or as an entry
 * point.
 */
public class Trip extends Transition {
    private static final String TAG = "Transit";
    private final int mId;

    @Nullable private final Station mOrigin;
    private final Station mDestination;

    private List<ConditionWait> mWaits;

    private static int sLastTripId;

    private Trip(
            @Nullable Station origin,
            Station destination,
            TransitionOptions options,
            Trigger trigger) {
        super(options, trigger);
        mOrigin = origin;
        mDestination = destination;
        mId = ++sLastTripId;
    }

    /**
     * Starts a transition from a {@link Station} to another (or from no {@link Station} if at an
     * entry point). Runs the transition |trigger|, and blocks until the destination {@link Station}
     * is considered ACTIVE (enter Conditions are fulfilled), the origin {@link Station} is
     * considered FINISHED (exit Conditions are fulfilled), and the {@link Trip}'s transition
     * conditions are fulfilled.
     *
     * @param origin the {@link Facility} to depart from, null if at an entry point.
     * @param destination the {@link Facility} to arrive at.
     * @param trigger the trigger to start the transition (e.g. clicking a view).
     * @return the destination {@link Station}, now ACTIVE.
     * @param <T> the type of the destination {@link Station}.
     */
    public static <T extends Station> T travelSync(
            @Nullable Station origin, T destination, Trigger trigger) {
        Trip trip = new Trip(origin, destination, TransitionOptions.DEFAULT, trigger);
        trip.travelSyncInternal();
        return destination;
    }

    /** Version of #travelSync() with extra TransitionOptions. */
    public static <T extends Station> T travelSync(
            @Nullable Station origin, T destination, TransitionOptions options, Trigger trigger) {
        Trip trip = new Trip(origin, destination, options, trigger);
        trip.travelSyncInternal();
        return destination;
    }

    private void travelSyncInternal() {
        // TODO(crbug.com/333735412): Unify Trip#travelSyncInternal(), FacilityCheckIn#enterSync()
        // and FacilityCheckOut#exitSync().
        embark();
        if (mOrigin != null) {
            Log.i(TAG, "Trip %d: Embarked at %s towards %s", mId, mOrigin, mDestination);
        } else {
            Log.i(TAG, "Trip %d: Starting at entry point %s", mId, mDestination);
        }

        if (mOptions.mTries == 1) {
            triggerTransition();
            Log.i(TAG, "Trip %d: Triggered transition, waiting to arrive at %s", mId, mDestination);
            waitUntilArrival();
        } else {
            for (int tryNumber = 1; tryNumber <= mOptions.mTries; tryNumber++) {
                try {
                    triggerTransition();
                    Log.i(
                            TAG,
                            "Trip %d: Triggered transition (try #%d/%d), waiting to arrive at %s",
                            mId,
                            tryNumber,
                            mOptions.mTries,
                            mDestination);
                    waitUntilArrival();
                    break;
                } catch (TravelException e) {
                    Log.w(TAG, "Try #%d failed", tryNumber, e);
                    if (tryNumber >= mOptions.mTries) {
                        throw e;
                    }
                }
            }
        }

        Log.i(TAG, "Trip %d: Arrived at %s", mId, mDestination);

        PublicTransitConfig.maybePauseAfterTransition(mDestination);
    }

    private void embark() {
        if (mOrigin != null) {
            mOrigin.setStateTransitioningFrom();
        }
        mDestination.setStateTransitioningTo();

        mWaits = calculateConditionWaits(mOrigin, mDestination, getTransitionConditions());
        for (ConditionWait wait : mWaits) {
            wait.getCondition().onStartMonitoring();
        }
    }

    private void waitUntilArrival() {
        // Throws CriteriaNotSatisfiedException if any conditions aren't met within the timeout and
        // prints the state of all conditions. The timeout can be reduced when explicitly looking
        // for flakiness due to tight timeouts.
        try {
            ConditionWaiter.waitFor(mWaits, mOptions);
        } catch (AssertionError e) {
            throw newTransitionException(e);
        }

        if (mOrigin != null) {
            mOrigin.setStateFinished();
        }
        mDestination.setStateActive();
        for (ConditionWait waits : mWaits) {
            waits.getCondition().onStopMonitoring();
        }

        TrafficControl.notifyActiveStationChanged(mDestination);
    }

    private static ArrayList<ConditionWait> calculateConditionWaits(
            @Nullable Station origin, Station destination, List<Condition> transitionConditions) {
        ArrayList<ConditionWait> waits = new ArrayList<>();

        Elements originElements =
                origin != null
                        ? origin.getElementsIncludingFacilitiesWithPhase(Phase.TRANSITIONING_FROM)
                        : Elements.EMPTY;
        Elements destinationElements =
                destination.getElementsIncludingFacilitiesWithPhase(Phase.TRANSITIONING_TO);

        // Create ENTER Conditions for Views that should appear and LogicalElements that should
        // be true.
        Set<String> destinationElementIds = new HashSet<>();
        for (ElementInState element : destinationElements.getElementsInState()) {
            destinationElementIds.add(element.getId());
            @Nullable Condition enterCondition = element.getEnterCondition();
            if (enterCondition != null) {
                waits.add(new ConditionWait(enterCondition, ConditionWaiter.ConditionOrigin.ENTER));
            }
        }

        // Add extra ENTER Conditions.
        for (Condition enterCondition : destinationElements.getOtherEnterConditions()) {
            waits.add(new ConditionWait(enterCondition, ConditionWaiter.ConditionOrigin.ENTER));
        }

        // Create EXIT Conditions for Views that should disappear and LogicalElements that should
        // be false.
        for (ElementInState element : originElements.getElementsInState()) {
            Condition exitCondition = element.getExitCondition(destinationElementIds);
            if (exitCondition != null) {
                waits.add(new ConditionWait(exitCondition, ConditionWaiter.ConditionOrigin.EXIT));
            }
        }

        // Add extra EXIT Conditions.
        for (Condition exitCondition : originElements.getOtherExitConditions()) {
            waits.add(new ConditionWait(exitCondition, ConditionWaiter.ConditionOrigin.EXIT));
        }

        // Add transition (TRSTN) conditions
        for (Condition condition : transitionConditions) {
            waits.add(new ConditionWait(condition, ConditionWaiter.ConditionOrigin.TRANSITION));
        }

        return waits;
    }

    @Override
    public String toDebugString() {
        return String.format(
                "Trip from %s to %s",
                mOrigin != null ? mOrigin.toString() : "<entry point>", mDestination);
    }
}
