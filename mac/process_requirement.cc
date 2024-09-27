// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/mac/process_requirement.h"

#include <Kernel/kern/cs_blobs.h>
#include <Security/Security.h>
#include <stdint.h>
#include <sys/errno.h>

#include <optional>

#include "base/apple/osstatus_logging.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/mac/code_signature.h"
#include "base/mac/code_signature_spi.h"
#include "base/mac/mac_util.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/ranges/algorithm.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/types/expected.h"

using base::apple::ScopedCFTypeRef;

namespace base::mac {

enum class ValidationCategory : unsigned int {
  Invalid = CS_VALIDATION_CATEGORY_INVALID,
  Platform = CS_VALIDATION_CATEGORY_PLATFORM,
  TestFlight = CS_VALIDATION_CATEGORY_TESTFLIGHT,
  Development = CS_VALIDATION_CATEGORY_DEVELOPMENT,
  AppStore = CS_VALIDATION_CATEGORY_APP_STORE,
  Enterprise = CS_VALIDATION_CATEGORY_ENTERPRISE,
  DeveloperId = CS_VALIDATION_CATEGORY_DEVELOPER_ID,
  LocalSigning = CS_VALIDATION_CATEGORY_LOCAL_SIGNING,
  Rosetta = CS_VALIDATION_CATEGORY_ROSETTA,
  OopJit = CS_VALIDATION_CATEGORY_OOPJIT,
  None = CS_VALIDATION_CATEGORY_NONE,
};

namespace {

// Requirements derived from the designated requirements described in TN3127:
// Inside Code Signing: Requirements
// (https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements).
constexpr std::string_view kAnyDeveloperIdRequirement =
    "(anchor apple generic and certificate "
    "1[field.1.2.840.113635.100.6.2.6] exists and certificate "
    "leaf[field.1.2.840.113635.100.6.1.13] exists)";
constexpr std::string_view kAnyAppStoreRequirement =
    "(anchor apple generic and certificate "
    "leaf[field.1.2.840.113635.100.6.1.9] exists)";
constexpr std::string_view kAnyDevelopmentRequirement =
    "(anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.1] "
    "exists)";

// A requirement string that will match ad-hoc signed code. It will also match
// code signed with non-Apple certificates, but those are not supported by
// `ProcessRequirement`.
constexpr std::string_view kNonAppleAnchorRequirement =
    "!(anchor apple generic)";

struct CSOpsSystemCallProviderImpl
    : ProcessRequirement::CSOpsSystemCallProvider {
  ~CSOpsSystemCallProviderImpl() override = default;

  int csops(pid_t pid,
            unsigned int ops,
            void* useraddr,
            size_t usersize) override {
    return ::csops(pid, ops, useraddr, usersize);
  }
};

ProcessRequirement::CSOpsSystemCallProvider* DefaultCSOpsProvider() {
  static NoDestructor<CSOpsSystemCallProviderImpl> default_provider;
  return default_provider.get();
}

ProcessRequirement::CSOpsSystemCallProvider*& CSOpsProvider() {
  static ProcessRequirement::CSOpsSystemCallProvider* provider =
      DefaultCSOpsProvider();
  return provider;
}

base::expected<std::string, int> TeamIdentifierOfCurrentProcess() {
  struct {
    uint32_t type;
    uint32_t length;
    char identifier[CS_MAX_TEAMID_LEN + 1];
  } result_data;
  int result = CSOpsProvider()->csops(getpid(), CS_OPS_TEAMID, &result_data,
                                      sizeof(result_data));
  if (result < 0) {
    if (errno == ENOENT) {
      // Process has no team identifier (ad-hoc signed, unsigned, etc).
      return "";
    }

    PLOG(ERROR) << "csops(CS_OPS_TEAMID) failed";
    return base::unexpected(errno);
  }

  return std::string(result_data.identifier);
}

base::expected<ValidationCategory, int> ValidationCategoryOfCurrentProcess() {
  ValidationCategory validation_category = ValidationCategory::Invalid;
  int result =
      CSOpsProvider()->csops(getpid(), CS_OPS_VALIDATION_CATEGORY,
                             &validation_category, sizeof(validation_category));
  if (result < 0) {
    PLOG(ERROR) << "csops(CS_OPS_VALIDATION_CATEGORY) failed";
    return base::unexpected(errno);
  }

  return validation_category;
}

// Determine the validation category of the current process by evaluating
// the current process's code signature against requirements that represent
// each of the validation categories we're interested in.
base::expected<ValidationCategory, OSStatus>
FallbackValidationCategoryOfCurrentProcess() {
  ScopedCFTypeRef<SecCodeRef> self_code;
  if (OSStatus status =
          SecCodeCopySelf(kSecCSDefaultFlags, self_code.InitializeInto())) {
    OSSTATUS_LOG(ERROR, status)
        << "Unable to derive validation category for current "
           "process. Failed to copy code object";
    return base::unexpected(status);
  }

  // Do initial validation without a requirement to detect problems with the
  // code signature itself. We do basic validation only as the validation is
  // secondary to requirement matching in this case.
  if (OSStatus status = SecStaticCodeCheckValidity(
          self_code.get(), kSecCSBasicValidateOnly, nullptr)) {
    if (status == errSecCSUnsigned) {
      return ValidationCategory::None;
    }

    OSSTATUS_LOG(ERROR, status)
        << "Unable to derive validation category for current "
           "process. Signature validation of current process failed";
    return base::unexpected(status);
  }

  std::pair<ValidationCategory, std::string_view> supported_categories[] = {
      {ValidationCategory::DeveloperId, kAnyDeveloperIdRequirement},
      {ValidationCategory::AppStore, kAnyAppStoreRequirement},
      {ValidationCategory::Development, kAnyDevelopmentRequirement},
      {ValidationCategory::None, kNonAppleAnchorRequirement},
  };

  for (auto& [category, requirement] : supported_categories) {
    OSStatus status =
        SecStaticCodeCheckValidity(self_code.get(), kSecCSBasicValidateOnly,
                                   RequirementFromString(requirement).get());
    switch (status) {
      case errSecSuccess:
        // Requirement matched so we now know the validation category.
        return category;

      case errSecCSReqFailed:
        // Requirement did not match. On to the next one.
        continue;

      default:
        OSSTATUS_LOG(INFO, status)
            << "Unexpected error when evaluating requirement " << requirement;
    }
  }

  LOG(ERROR) << "Unable to derive validation category for current process. "
                "Signature did not match any supported requirement.";
  return base::unexpected(errSecFunctionFailed);
}

std::string RequirementStringForValidationCategory(
    ValidationCategory category) {
  // It is not meaningful to create a requirement string for an unsigned or
  // ad-hoc signed process.
  CHECK_NE(category, ValidationCategory::None);

  switch (category) {
    case ValidationCategory::DeveloperId:
      return std::string(kAnyDeveloperIdRequirement);
    case ValidationCategory::AppStore:
      return std::string(kAnyAppStoreRequirement);
    case ValidationCategory::Development:
      return std::string(kAnyDevelopmentRequirement);
    default:
      NOTREACHED() << "Unsupported process validation category: "
                   << static_cast<unsigned int>(category);
  }
}

}  // namespace

ProcessRequirement::Builder::Builder() = default;
ProcessRequirement::Builder::~Builder() = default;

ProcessRequirement::Builder::Builder(Builder&&) = default;
ProcessRequirement::Builder& ProcessRequirement::Builder::operator=(Builder&&) =
    default;

ProcessRequirement::Builder ProcessRequirement::Builder::Identifier(
    std::string identifier) && {
  CHECK(identifier.size());
  CHECK(identifiers_.empty());
  identifiers_.push_back(std::move(identifier));
  return std::move(*this);
}

ProcessRequirement::Builder ProcessRequirement::Builder::IdentifierIsOneOf(
    std::vector<std::string> identifiers) && {
  CHECK(identifiers.size());
  CHECK(base::ranges::all_of(identifiers, &std::string::size));
  CHECK(identifiers_.empty());
  identifiers_ = std::move(identifiers);
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::SignedWithSameIdentity() && {
  return std::move(*this).HasSameTeamIdentifier().HasSameCertificateType();
}

ProcessRequirement::Builder
ProcessRequirement::Builder::HasSameTeamIdentifier() && {
  CHECK(team_identifier_.empty());
  if (auto team_identifier = TeamIdentifierOfCurrentProcess();
      team_identifier.has_value()) {
    team_identifier_ = std::move(*team_identifier);
    return std::move(*this);
  }

  LOG(ERROR) << "HasSameTeamIdentifier failed to retrieve team identifier of "
                "current process";
  failed_ = true;
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::HasSameCertificateType() && {
  CHECK(!validation_category_);

  if (MacOSMajorVersion() >= 13) {
    auto validation_category = ValidationCategoryOfCurrentProcess();
    if (validation_category.has_value()) {
      validation_category_ = *validation_category;
    } else {
      failed_ = true;
    }
  } else {
    // Older macOS versions do not support CS_OPS_VALIDATION_CATEGORY. Derive
    // the validation category via Security.framework instead.
    static auto validation_category =
        FallbackValidationCategoryOfCurrentProcess();
    if (validation_category.has_value()) {
      validation_category_ = *validation_category;
    } else {
      failed_ = true;
    }
  }

  return std::move(*this);
}

ProcessRequirement::Builder ProcessRequirement::Builder::TeamIdentifier(
    std::string team_identifier) && {
  CHECK(team_identifier_.empty());
  CHECK(base::ranges::all_of(team_identifier, base::IsAsciiAlphaNumeric<char>));
  team_identifier_ = std::move(team_identifier);
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::DeveloperIdCertificateType() && {
  validation_category_ = ValidationCategory::DeveloperId;
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::AppStoreCertificateType() && {
  validation_category_ = ValidationCategory::AppStore;
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::DevelopmentCertificateType() && {
  validation_category_ = ValidationCategory::Development;
  return std::move(*this);
}

ProcessRequirement::Builder
ProcessRequirement::Builder::CheckDynamicValidityOnly() && {
  dynamic_validity_only_ = true;
  return std::move(*this);
}

std::optional<ProcessRequirement> ProcessRequirement::Builder::Build() && {
  if (failed_) {
    VLOG(2)
        << "ProcessRequirement::Builder::Build: failed validation -> nullopt";
    return std::nullopt;
  }

  ValidationCategory validation_category =
      validation_category_.value_or(ValidationCategory::None);

  if (validation_category == ValidationCategory::None) {
    CHECK(team_identifier_.empty())
        << "A process requirement matching on a team identifier without "
           "specifying a certificate type is unsafe.";
  } else {
    CHECK(team_identifier_.size())
        << "A process requirement without a team identifier is unsafe as it "
           "can be matched by any signing identity of that type.";
  }

  return ProcessRequirement(std::move(identifiers_),
                            std::move(team_identifier_), validation_category,
                            dynamic_validity_only_);
}

ProcessRequirement::ProcessRequirement(std::vector<std::string> identifiers,
                                       std::string team_identifier,
                                       ValidationCategory validation_category,
                                       bool dynamic_validity_only)
    : identifiers_(std::move(identifiers)),
      team_identifier_(std::move(team_identifier)),
      validation_category_(validation_category),
      dynamic_validity_only_(dynamic_validity_only) {
  CHECK(validation_category_ != ValidationCategory::Invalid);
}

ProcessRequirement::ProcessRequirement(ForTesting for_testing)
    : for_testing_(for_testing),
      validation_category_(ValidationCategory::Invalid) {}

ProcessRequirement::~ProcessRequirement() = default;

ProcessRequirement::ProcessRequirement(const ProcessRequirement&) = default;
ProcessRequirement& ProcessRequirement::operator=(const ProcessRequirement&) =
    default;

ProcessRequirement::ProcessRequirement(ProcessRequirement&&) = default;
ProcessRequirement& ProcessRequirement::operator=(ProcessRequirement&&) =
    default;

// static
ProcessRequirement ProcessRequirement::AlwaysMatchesForTesting() {
  return ProcessRequirement(ForTesting::AlwaysMatches);
}

// static
ProcessRequirement ProcessRequirement::NeverMatchesForTesting() {
  return ProcessRequirement(ForTesting::NeverMatches);
}

bool ProcessRequirement::RequiresSignatureValidation() const {
  if (for_testing_.has_value()) {
    // `ForTesting::AlwaysMatches` does not require validation because
    // a test process is likely to be unsigned.
    // `ForTesting::NeverMatches` will fail signature validation with
    // `errSecCSUnsigned` if the process is unsigned, and will fail requirement
    // evaluation if the process has a valid ad-hoc signature.
    return for_testing_.value() == ForTesting::NeverMatches;
  }

  // All validation categories besides none (ad-hoc signature or unsigned)
  // require validation.
  // It is not useful to validate an ad-hoc signature as anyone can create
  // an ad-hoc signature that matches this requirement.
  return validation_category_ != ValidationCategory::None;
}

ScopedCFTypeRef<SecRequirementRef> ProcessRequirement::AsSecRequirement()
    const {
  if (for_testing_.has_value()) {
    return AsSecRequirementForTesting(for_testing_.value());  // IN-TEST
  }

  if (!RequiresSignatureValidation()) {
    VLOG(2) << "ProcessRequirement::AsSecRequirement -> nullptr";
    return ScopedCFTypeRef<SecRequirementRef>{nullptr};
  }

  std::vector<std::string> clauses;

  if (identifiers_.size()) {
    std::vector<std::string> identifier_clauses;
    for (const std::string& identifier : identifiers_) {
      identifier_clauses.push_back(StrCat({"identifier \"", identifier, "\""}));
    }
    if (identifier_clauses.size() == 1) {
      clauses.push_back(std::move(identifier_clauses.front()));
    } else {
      std::string identifier_clause =
          base::JoinString(identifier_clauses, " or ");
      clauses.push_back(StrCat({"(", identifier_clause, ")"}));
    }
  }

  if (team_identifier_.size()) {
    clauses.push_back(
        StrCat({"certificate leaf[subject.OU] = ", team_identifier_}));
  }

  clauses.push_back(
      RequirementStringForValidationCategory(validation_category_));

  std::string requirement_string = base::JoinString(clauses, " and ");
  VLOG(2) << "ProcessRequirement::AsSecRequirement -> " << requirement_string;
  apple::ScopedCFTypeRef<SecRequirementRef> requirement =
      RequirementFromString(requirement_string);
  CHECK(requirement) << "ProcessRequirement::AsSecRequirement generated a "
                        "requirement string that could not be parsed.";
  return requirement;
}

// static
ScopedCFTypeRef<SecRequirementRef>
ProcessRequirement::AsSecRequirementForTesting(
    ProcessRequirement::ForTesting for_testing) {
  std::string requirement_string;
  switch (for_testing) {
    case ForTesting::AlwaysMatches: {
      requirement_string = "(!info[ThisKeyDoesNotExist])";
      break;
    }
    case ForTesting::NeverMatches: {
      requirement_string = R"(identifier = "this is not the identifier")";
      break;
    }
  }
  ScopedCFTypeRef<SecRequirementRef> requirement =
      RequirementFromString(requirement_string);
  CHECK(requirement)
      << "ProcessRequirement::AsSecRequirementForTesting generated a "
         "requirement string that could not be parsed.";
  return requirement;
}

// static
void ProcessRequirement::SetCSOpsSystemCallProviderForTesting(
    CSOpsSystemCallProvider* csops_provider) {
  if (csops_provider) {
    CSOpsProvider() = csops_provider;
  } else {
    CSOpsProvider() = DefaultCSOpsProvider();
  }
}

bool ProcessRequirement::ValidateProcess(
    audit_token_t audit_token,
    base::span<const uint8_t> info_plist_data) const {
  if (!RequiresSignatureValidation()) {
    // No signature validation required. Return success.
    return true;
  }

  // If the requirement specifies we are checking only the validity of the
  // dynamic code then we must have Info.plist data.
  if (dynamic_validity_only_) {
    CHECK(info_plist_data.size())
        << "info_plist_data is required when checking dynamic validity only.";
  }

  if (OSStatus status = ProcessIsSignedAndFulfillsRequirement(
          audit_token, AsSecRequirement().get(),
          dynamic_validity_only_ ? SignatureValidationType::DynamicOnly
                                 : SignatureValidationType::DynamicAndStatic,
          base::as_string_view(info_plist_data))) {
    OSSTATUS_LOG(ERROR, status) << "ProcessIsSignedAndFulfillsRequirement";
    return false;
  }

  return true;
}

}  // namespace base::mac
