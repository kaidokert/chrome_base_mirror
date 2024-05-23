// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base.test;

import android.app.Application;
import android.app.job.JobScheduler;
import android.content.Context;
import android.os.Bundle;
import android.os.SystemClock;

import androidx.annotation.CallSuper;
import androidx.test.InstrumentationRegistry;
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner;
import androidx.test.internal.util.AndroidRunnerParams;

import org.junit.rules.MethodRule;
import org.junit.rules.TestRule;
import org.junit.runner.Description;
import org.junit.runner.notification.RunNotifier;
import org.junit.runners.model.FrameworkMethod;
import org.junit.runners.model.InitializationError;
import org.junit.runners.model.Statement;

import org.chromium.base.FeatureParam;
import org.chromium.base.Flag;
import org.chromium.base.LifetimeAssert;
import org.chromium.base.Log;
import org.chromium.base.ResettersForTesting;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.metrics.UmaRecorderHolder;
import org.chromium.base.test.params.MethodParamAnnotationRule;
import org.chromium.base.test.util.AndroidSdkLevelSkipCheck;
import org.chromium.base.test.util.BaseRestrictions;
import org.chromium.base.test.util.Batch;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.DisableIfSkipCheck;
import org.chromium.base.test.util.RequiresRestart;
import org.chromium.base.test.util.RestrictionSkipCheck;
import org.chromium.base.test.util.SkipCheck;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * A custom runner for JUnit4 tests that checks requirements to conditionally ignore tests.
 *
 * <p>This ClassRunner imports from AndroidJUnit4ClassRunner which is a hidden but accessible class.
 * The reason is that default JUnit4 runner for Android is a final class, AndroidJUnit4. We need to
 * extends an inheritable class to change {@link #runChild} and {@link #isIgnored} to add SkipChecks
 * and PreTesthook.
 */
public class BaseJUnit4ClassRunner extends AndroidJUnit4ClassRunner {
    private static final String TAG = "BaseJUnit4ClassRunnr";

    // Arbirary int that must not overlap with status codes defined by
    // https://developer.android.com/reference/android/test/InstrumentationTestRunner.html#REPORT_VALUE_ID
    private static final int STATUS_CODE_TEST_DURATION = 1337;
    private static final String DURATION_BUNDLE_ID = "duration_ms";

    /**
     * An interface for classes that have some code to run before (or after) the class is
     * instantiated. They run after {@Link BeforeClass} (or before @AfterClass) methods are called.
     * Provides access to the test class (and the annotations defined for it) and the
     * instrumentation context.
     *
     * <p>The only reason to use a ClassHook instead of a TestRule is
     * because @BeforeClass/@AfterClass run during test listing, or multiple times for parameterized
     * tests. See https://crbug.com/1090043.
     */
    public interface ClassHook {
        /**
         * @param targetContext the instrumentation context that will be used during the test.
         * @param testMethod the test method to be run.
         */
        public void run(Context targetContext, Class<?> testClass);
    }

    /**
     * An interface for classes that have some code to run before a test. They run after
     * {@link SkipCheck}s and before {@Link Before} (or after @After). Provides access to the test
     * method (and the annotations defined for it) and the instrumentation context.
     *
     * Do not use TestHooks unless you also require ClassHooks. Otherwise, you should use TestRules
     * and {@link #getDefaultTestRules}.
     */
    public interface TestHook {
        /**
         * @param targetContext the instrumentation context that will be used during the test.
         * @param testMethod the test method to be run.
         */
        public void run(Context targetContext, FrameworkMethod testMethod);
    }

    protected final RestrictionSkipCheck mRestrictionSkipCheck = new RestrictionSkipCheck();
    private long mTestStartTimeMs;

    /**
     * Create a BaseJUnit4ClassRunner to run {@code klass} and initialize values.
     *
     * @throws InitializationError if the test class malformed
     */
    public BaseJUnit4ClassRunner(final Class<?> klass) throws InitializationError {
        super(
                klass,
                new AndroidRunnerParams(
                        InstrumentationRegistry.getInstrumentation(),
                        InstrumentationRegistry.getArguments(),
                        false,
                        0L,
                        false));

        BaseRestrictions.registerChecks(mRestrictionSkipCheck);
        assert InstrumentationRegistry.getInstrumentation()
                        instanceof BaseChromiumAndroidJUnitRunner
                : "Must use BaseChromiumAndroidJUnitRunner instrumentation with "
                        + "BaseJUnit4ClassRunner, but found: "
                        + InstrumentationRegistry.getInstrumentation().getClass();
    }

    /** Returns the singleton Application instance. */
    public static Application getApplication() {
        return BaseChromiumAndroidJUnitRunner.sApplication;
    }

    /**
     * Merge two List into a new ArrayList.
     *
     * <p>Used to merge the default SkipChecks/PreTestHooks with the subclasses's
     * SkipChecks/PreTestHooks.
     */
    private static <T> List<T> mergeList(List<T> listA, List<T> listB) {
        List<T> l = new ArrayList<>(listA);
        l.addAll(listB);
        return l;
    }

    @SafeVarargs
    protected static <T> List<T> addToList(List<T> list, T... additionalEntries) {
        return mergeList(list, Arrays.asList(additionalEntries));
    }

    @Override
    protected void collectInitializationErrors(List<Throwable> errors) {
        super.collectInitializationErrors(errors);
        // Log any initialization errors to help debugging, as the host-side test runner can get
        // confused by the thrown exception.
        if (!errors.isEmpty()) {
            Log.e(TAG, "Initialization errors in %s: %s", getTestClass().getName(), errors);
        }
    }

    /**
     * Override this method to return a list of {@link SkipCheck}s}.
     *
     * <p>Additional hooks can be added to the list using {@link #addToList}: {@code return
     * addToList(super.getSkipChecks(), check1, check2);}
     */
    @CallSuper
    protected List<SkipCheck> getSkipChecks() {
        return Arrays.asList(
                mRestrictionSkipCheck, new AndroidSdkLevelSkipCheck(), new DisableIfSkipCheck());
    }

    /**
     * See {@link ClassHook}.
     *
     * <p>Additional hooks can be added to the list by overriding this method and using {@link
     * #addToList}: {@code return addToList(super.getPreClassHooks(), hook1, hook2);}
     */
    @CallSuper
    protected List<ClassHook> getPreClassHooks() {
        return Collections.emptyList();
    }

    /**
     * See {@link TestHook}.
     *
     * <p>Additional hooks can be added to the list by overriding this method and using {@link
     * #addToList}: {@code return addToList(super.getPreTestHooks(), hook1, hook2);}
     */
    @CallSuper
    protected List<TestHook> getPreTestHooks() {
        return Collections.emptyList();
    }

    /**
     * See {@link TestHook}.
     *
     * <p>Additional hooks can be added to the list by overriding this method and using {@link
     * #addToList}: {@code return addToList(super.getPostTestHooks(), hook1, hook2);}
     */
    @CallSuper
    protected List<TestHook> getPostTestHooks() {
        return Collections.emptyList();
    }

    /**
     * Override this method to return a list of method rules that should be applied to all tests run
     * with this test runner.
     *
     * <p>Additional rules can be added to the list using {@link #addToList}: {@code return
     * addToList(super.getDefaultMethodRules(), rule1, rule2);}
     */
    @CallSuper
    protected List<MethodRule> getDefaultMethodRules() {
        return Collections.singletonList(new MethodParamAnnotationRule());
    }

    /**
     * Override this method to return a list of rules that should be applied to all tests run with
     * this test runner.
     *
     * <p>Additional rules can be added to the list using {@link #addToList}: {@code return
     * addToList(super.getDefaultTestRules(), rule1, rule2);}
     */
    @CallSuper
    protected List<TestRule> getDefaultTestRules() {
        return Collections.emptyList();
    }

    /** Evaluate whether a FrameworkMethod is ignored based on {@code SkipCheck}s. */
    @Override
    protected boolean isIgnored(FrameworkMethod method) {
        return super.isIgnored(method) || shouldSkip(method);
    }

    @Override
    protected List<MethodRule> rules(Object target) {
        List<MethodRule> declaredRules = super.rules(target);
        List<MethodRule> defaultRules = getDefaultMethodRules();
        return mergeList(defaultRules, declaredRules);
    }

    @Override
    protected final List<TestRule> getTestRules(Object target) {
        List<TestRule> declaredRules = super.getTestRules(target);
        List<TestRule> defaultRules = getDefaultTestRules();
        return mergeList(declaredRules, defaultRules);
    }

    /** Run test with or without execution based on bundle arguments. */
    @Override
    public void run(RunNotifier notifier) {
        if (BaseChromiumAndroidJUnitRunner.shouldListTests()) {
            for (Description child : getDescription().getChildren()) {
                notifier.fireTestFinished(child);
            }
            return;
        }

        onBeforeTestClass();
        super.run(notifier);
        onAfterTestClass();
    }

    static void clearJobSchedulerJobs() {
        BaseJUnit4ClassRunner.getApplication().getSystemService(JobScheduler.class).cancelAll();
    }

    private static void blockUnitTestsFromStartingBrowser(FrameworkMethod testMethod) {
        Batch annotation = testMethod.getDeclaringClass().getAnnotation(Batch.class);
        if (annotation != null && annotation.value().equals(Batch.UNIT_TESTS)) {
            if (testMethod.getAnnotation(RequiresRestart.class) != null) return;
            LibraryLoader.setBrowserProcessStartupBlockedForTesting();
        }
    }

    @Override
    protected Statement methodBlock(final FrameworkMethod method) {
        Statement innerStatement = super.methodBlock(method);
        return new Statement() {
            @Override
            public void evaluate() throws Throwable {
                onBeforeTestMethod(method);
                Throwable exception = null;
                try {
                    // Runs @Rules, then @Befores, then test method.
                    innerStatement.evaluate();
                    performExtraAssertions(method);
                } catch (Throwable t) {
                    exception = t;
                }
                try {
                    onAfterTestMethod(method);
                } catch (Throwable t) {
                    // Ensure original exception is not lost if onAfterTestMethod() throws.
                    if (exception != null) {
                        Log.e(TAG, "Unexpected exception in onAfterTestMethod()", t);
                    } else {
                        exception = t;
                    }
                }
                if (exception != null) {
                    throw exception;
                }
            }
        };
    }

    private void onBeforeTestMethod(FrameworkMethod method) {
        TestTraceEvent.begin(method.getName());

        mTestStartTimeMs = SystemClock.uptimeMillis();
        ResettersForTesting.beforeHooksWillExecute();

        // TODO: Might be slow to do this before every test.
        SharedPreferencesTestUtil.deleteOnDiskSharedPreferences(getApplication());

        CommandLineFlags.setUpMethod(method.getMethod());
        blockUnitTestsFromStartingBrowser(method);
        // TODO(agrieve): These should not reset flag values set in @BeforeClass
        Flag.resetAllInMemoryCachedValuesForTesting();
        FeatureParam.resetAllInMemoryCachedValuesForTesting();
        UmaRecorderHolder.resetForTesting();

        Context targetContext = InstrumentationRegistry.getTargetContext();
        for (TestHook hook : getPreTestHooks()) {
            hook.run(targetContext, method);
        }
    }

    private void onBeforeTestClass() {
        Class<?> testClass = getTestClass().getJavaClass();
        ResettersForTesting.beforeClassHooksWillExecute();
        CommandLineFlags.setUpClass(testClass);

        Context targetContext = InstrumentationRegistry.getTargetContext();
        for (ClassHook hook : getPreClassHooks()) {
            hook.run(targetContext, testClass);
        }
    }

    private void performExtraAssertions(FrameworkMethod method) throws Throwable {
        SharedPreferencesTestUtil.assertNoOnDiskSharedPreferences();

        Batch annotation = method.getDeclaringClass().getAnnotation(Batch.class);
        if (annotation != null && annotation.value().equals(Batch.UNIT_TESTS)) {
            if (method.getAnnotation(RequiresRestart.class) != null) return;
            LifetimeAssert.assertAllInstancesDestroyedForTesting();
        }
    }

    protected void onAfterTestMethod(FrameworkMethod method) {
        Context targetContext = InstrumentationRegistry.getTargetContext();
        for (TestHook hook : getPostTestHooks()) {
            hook.run(targetContext, method);
        }

        // Do not reset things here for state we may want to persist when set via @BeforeClass.
        CommandLineFlags.tearDownMethod();
        try {
            // Run resetters on UI thread so as to minimize the number of failed thread check
            // assertions, and to match the semantics of Robolectric's runners.
            BaseChromiumAndroidJUnitRunner.sInstance.runOnMainSync(
                    ResettersForTesting::afterHooksDidExecute);
            clearJobSchedulerJobs();
        } finally {
            Bundle b = new Bundle();
            b.putLong(DURATION_BUNDLE_ID, SystemClock.uptimeMillis() - mTestStartTimeMs);
            InstrumentationRegistry.getInstrumentation().sendStatus(STATUS_CODE_TEST_DURATION, b);

            TestTraceEvent.end(method.getName());

            // TODO: This dumps trace output after a single test, which means traces do not work for
            // @Batched tests.
            TestTraceEvent.disable();
        }
    }

    protected void onAfterTestClass() {
        CommandLineFlags.tearDownClass();
        // Run resetters on UI thread so as to minimize the number of failed thread check
        // assertions, and to match the semantics of Robolectric's runners.
        BaseChromiumAndroidJUnitRunner.sInstance.runOnMainSync(
                ResettersForTesting::afterClassHooksDidExecute);
    }

    /** Loop through all the {@code SkipCheck}s to confirm whether a test should be ignored */
    private boolean shouldSkip(FrameworkMethod method) {
        for (SkipCheck s : getSkipChecks()) {
            if (s.shouldSkip(method)) {
                return true;
            }
        }
        return false;
    }

    /** Overriding this method to take screenshot of failure before tear down functions are run. */
    @Override
    protected Statement withAfters(FrameworkMethod method, Object test, Statement base) {
        // Afters are called before @Rule tearDown, so a good time for a screenshot.
        return super.withAfters(method, test, new ScreenshotOnFailureStatement(base));
    }
}
