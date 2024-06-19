// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base.test.transit;

import androidx.annotation.Nullable;

import org.chromium.base.supplier.Supplier;

import java.util.Set;

/**
 * Represents an Element added to a {@link ConditionalState} that supplies a {@param <ProductT>}.
 *
 * @param <ProductT> the type of object supplied when this Element is present.
 */
public abstract class ElementInState<ProductT> implements Supplier<ProductT> {

    /**
     * @return an id used to identify during a {@link Transition} if the same element is a part of
     *     both the origin and destination {@link ConditionalState}s.
     */
    public abstract String getId();

    /**
     * @return an ENTER Condition to ensure the element is present in the ConditionalState.
     */
    public abstract ConditionWithResult<ProductT> getEnterCondition();

    /**
     * @param destinationElementIds ids of the elements in the destination for matching
     * @return an EXIT Condition to ensure the element is not present after transitioning to the
     *     destination.
     */
    public abstract @Nullable Condition getExitCondition(Set<String> destinationElementIds);

    @Override
    public ProductT get() {
        return getEnterCondition().get();
    }

    @Override
    public boolean hasValue() {
        return getEnterCondition().hasValue();
    }
}
