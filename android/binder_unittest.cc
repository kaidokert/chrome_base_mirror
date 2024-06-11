// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/android/binder.h"

#include <android/binder_status.h>

#include <atomic>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/notreached.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/bind.h"
#include "base/types/expected_macros.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base::android {
namespace {

class BinderTest : public testing::Test {
 public:
  void SetUp() override {
    if (!IsNativeBinderAvailable()) {
      GTEST_SKIP() << "This test is only meaningful when run on Android Q+ "
                   << "with the binder NDK library available.";
    }
  }
};

class AddInterface {
 public:
  DEFINE_BINDER_CLASS(Class);

  static constexpr transaction_code_t kAdd = 1234;

  class Proxy : public Class::BinderRef {
   public:
    explicit Proxy(BinderRef binder) : Class::BinderRef(std::move(binder)) {}

    int32_t Add(int32_t n) {
      const Parcel sum =
          *Transact(kAdd, [n](const auto& p) { return p.WriteInt32(n); });
      return *sum.reader().ReadInt32();
    }
  };
};

// Test service which adds a fixed offset to any transacted values;
class AddService : public SupportsBinder<AddInterface::Class> {
 public:
  explicit AddService(int32_t offset) : offset_(offset) {}

  void set_destruction_callback(base::OnceClosure callback) {
    destruction_callback_ = std::move(callback);
  }

  void set_binder_destruction_callback(base::RepeatingClosure callback) {
    binder_destruction_callback_ = std::move(callback);
  }

  // SupportsBinder<AddInterface::Class>:
  BinderStatusOr<void> OnBinderTransaction(transaction_code_t code,
                                           const ParcelReader& in,
                                           const ParcelWriter& out) override {
    EXPECT_EQ(AddInterface::kAdd, code);
    return out.WriteInt32(*in.ReadInt32() + offset_);
  }

  void OnBinderDestroyed() override {
    if (binder_destruction_callback_) {
      binder_destruction_callback_.Run();
    }
  }

 private:
  ~AddService() override {
    if (destruction_callback_) {
      std::move(destruction_callback_).Run();
    }
  }

  const int32_t offset_;

  base::OnceClosure destruction_callback_;
  base::RepeatingClosure binder_destruction_callback_;
};

TEST_F(BinderTest, BasicTransaction) {
  auto add42_service = base::MakeRefCounted<AddService>(42);
  AddInterface::Proxy add42(add42_service->GetBinder());
  EXPECT_EQ(47, add42.Add(5));
}

TEST_F(BinderTest, Lifecycle) {
  auto add42_service = base::MakeRefCounted<AddService>(42);
  AddInterface::Proxy add42(add42_service->GetBinder());

  bool is_destroyed = false;
  base::WaitableEvent destruction;
  add42_service->set_destruction_callback(base::BindLambdaForTesting([&] {
    is_destroyed = true;
    destruction.Signal();
  }));
  add42_service.reset();

  EXPECT_FALSE(is_destroyed);

  EXPECT_EQ(47, add42.Add(5));
  add42.reset();
  destruction.Wait();

  EXPECT_TRUE(is_destroyed);
}

TEST_F(BinderTest, OnBinderDestroyed) {
  auto add5_service = base::MakeRefCounted<AddService>(5);

  bool has_binder = true;
  base::WaitableEvent binder_destruction;
  add5_service->set_binder_destruction_callback(base::BindLambdaForTesting([&] {
    has_binder = false;
    binder_destruction.Signal();
  }));

  AddInterface::Proxy add5(add5_service->GetBinder());
  EXPECT_TRUE(has_binder);
  EXPECT_EQ(12, add5.Add(7));
  add5.reset();
  binder_destruction.Wait();
  EXPECT_FALSE(has_binder);

  binder_destruction.Reset();
  has_binder = true;

  AddInterface::Proxy add5_1(add5_service->GetBinder());
  AddInterface::Proxy add5_2(add5_service->GetBinder());
  EXPECT_EQ(6, add5_1.Add(1));
  EXPECT_EQ(7, add5_2.Add(2));

  add5_1.reset();
  EXPECT_TRUE(has_binder);
  add5_2.reset();
  binder_destruction.Wait();
  EXPECT_FALSE(has_binder);
}

class MultiplyInterface {
 public:
  DEFINE_BINDER_CLASS(Class);

  static constexpr transaction_code_t kMultiply = 5678;

  class Proxy : public Class::BinderRef {
   public:
    explicit Proxy(BinderRef binder) : Class::BinderRef(std::move(binder)) {}

    int32_t Multiply(int32_t n) {
      const Parcel product =
          *Transact(kMultiply, [n](const auto& p) { return p.WriteInt32(n); });
      return *product.reader().ReadInt32();
    }
  };
};

// Test service which multiplies transacted values by a fixed scale.
class MultiplyService : public SupportsBinder<MultiplyInterface::Class> {
 public:
  explicit MultiplyService(int32_t scale) : scale_(scale) {}

  void set_destruction_callback(base::OnceClosure callback) {
    destruction_callback_ = std::move(callback);
  }

  // SupportsBinder<MultiplyInterface::Class>:
  BinderStatusOr<void> OnBinderTransaction(transaction_code_t code,
                                           const ParcelReader& in,
                                           const ParcelWriter& out) override {
    EXPECT_EQ(MultiplyInterface::kMultiply, code);
    return out.WriteInt32(*in.ReadInt32() * scale_);
  }

 private:
  ~MultiplyService() override {
    if (destruction_callback_) {
      std::move(destruction_callback_).Run();
    }
  }

  const int32_t scale_;

  base::OnceClosure destruction_callback_;
};

TEST_F(BinderTest, MultipleInstances) {
  auto add100_service = base::MakeRefCounted<AddService>(100);
  auto add200_service = base::MakeRefCounted<AddService>(200);
  AddInterface::Proxy add100(add100_service->GetBinder());
  AddInterface::Proxy add200(add200_service->GetBinder());
  EXPECT_EQ(105, add100.Add(5));
  EXPECT_EQ(207, add200.Add(7));
}

TEST_F(BinderTest, MultipleClasses) {
  auto add100_service = base::MakeRefCounted<AddService>(100);
  auto multiply7_service = base::MakeRefCounted<MultiplyService>(7);
  AddInterface::Proxy add100(add100_service->GetBinder());
  MultiplyInterface::Proxy multiply7(multiply7_service->GetBinder());
  EXPECT_EQ(105, add100.Add(5));
  EXPECT_EQ(63, multiply7.Multiply(9));
}

class MathInterface {
 public:
  DEFINE_BINDER_CLASS(Class);

  static constexpr transaction_code_t kGetAdd = 1;
  static constexpr transaction_code_t kGetMultiply = 2;

  class Proxy : public Class::BinderRef {
   public:
    explicit Proxy(BinderRef binder) : Class::BinderRef(std::move(binder)) {}

    AddInterface::Proxy GetAdd(int32_t offset) {
      auto reply = *Transact(
          kGetAdd, [offset](const auto& p) { return p.WriteInt32(offset); });
      return AddInterface::Proxy(*reply.reader().ReadBinder());
    }

    MultiplyInterface::Proxy GetMultiply(int32_t scale) {
      auto reply = *Transact(
          kGetMultiply, [scale](const auto& p) { return p.WriteInt32(scale); });
      return MultiplyInterface::Proxy(*reply.reader().ReadBinder());
    }
  };
};

// A service which expects transactions requesting new AddInterface or
// MultiplyInterface binders with a respective offset or scale. Each request
// returns a binder for a bespoke service instance configured accordingly.
class MathService : public SupportsBinder<MathInterface::Class> {
 public:
  // SupportsBinder<MathInterface::Class>:
  BinderStatusOr<void> OnBinderTransaction(transaction_code_t code,
                                           const ParcelReader& in,
                                           const ParcelWriter& out) override {
    ASSIGN_OR_RETURN(const int32_t value, in.ReadInt32());
    switch (code) {
      case MathInterface::kGetAdd: {
        auto service = base::MakeRefCounted<AddService>(value);
        RETURN_IF_ERROR(out.WriteBinder(service->GetBinder()));
        service->set_destruction_callback(MakeNewServiceDestructionCallback());
        break;
      }

      case MathInterface::kGetMultiply: {
        auto service = base::MakeRefCounted<MultiplyService>(value);
        RETURN_IF_ERROR(out.WriteBinder(service->GetBinder()));
        service->set_destruction_callback(MakeNewServiceDestructionCallback());
        break;
      }

      default:
        NOTREACHED_NORETURN();
    }
    return base::ok();
  }

  void WaitForAllServicesToBeDestroyed() { all_services_destroyed_.Wait(); }

 private:
  base::OnceClosure MakeNewServiceDestructionCallback() {
    num_service_instances_.fetch_add(1, std::memory_order_relaxed);
    return base::BindLambdaForTesting([this] {
      if (num_service_instances_.fetch_sub(1, std::memory_order_relaxed) == 1) {
        all_services_destroyed_.Signal();
      }
    });
  }

  ~MathService() override = default;

  std::atomic_int num_service_instances_{0};
  base::WaitableEvent all_services_destroyed_;
};

TEST_F(BinderTest, BindersInTransactions) {
  auto math_service = base::MakeRefCounted<MathService>();
  MathInterface::Proxy math(math_service->GetBinder());

  auto add2 = math.GetAdd(2);
  auto multiply3 = math.GetMultiply(3);
  EXPECT_EQ(8002, add2.Add(8000));
  EXPECT_EQ(27000, multiply3.Multiply(9000));
  add2.reset();
  multiply3.reset();

  math_service->WaitForAllServicesToBeDestroyed();
}

}  // namespace
}  // namespace base::android
