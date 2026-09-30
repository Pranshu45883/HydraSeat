#include "hydra/update_transaction.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {
using namespace hydra;
using namespace hydra::installer;
using namespace hydra::update;

constexpr std::string_view kPublisher = "0123456789ABCDEF0123456789ABCDEF01234567";
constexpr std::string_view kWrongPublisher = "89ABCDEF0123456789ABCDEF0123456789ABCDEF";

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct C { OwnedComponent component; const char* id; const char* cap; };
constexpr std::array<C, 7> cs{{
    {OwnedComponent::MainUi,"hydraseat-main-ui","main-ui"},{OwnedComponent::Host,"hydraseat-host","runtime-host"},
    {OwnedComponent::SeatUi,"hydraseat-seat-ui","seat-ui"},{OwnedComponent::Watchdog,"hydraseat-watchdog","watchdog"},
    {OwnedComponent::Reset,"hydraseat-reset","recovery-reset"},{OwnedComponent::ProfileCli,"hydraseat-profile-cli","profile-cli"},
    {OwnedComponent::CommunityValidator,"hydraseat-community-validator","community-validator"}}};

InstallerPackage pkg(std::string version, std::uint64_t revision) {
    InstallerPackage p; p.releaseVersion = std::move(version); p.releaseRevision = revision;
    for (std::size_t i=0;i<cs.size();++i) {
        PackageComponent c; c.component=cs[i].component; c.manifest.artifactId=cs[i].id;
        c.manifest.artifactClass=trust::ArtifactClass::Executable; c.manifest.artifactVersion=p.releaseVersion;
        c.manifest.architecture=trust::ArtifactArchitecture::X64; c.manifest.expectedSha256=std::string(64u,'a'); c.manifest.expectedSha256[0]=static_cast<char>('0'+i);
        c.manifest.sourceId="hydraseat-release"; c.manifest.licenseId="hydraseat-project"; c.manifest.redistributionAllowed=true;
        c.manifest.optional=false; c.manifest.requiresInstall=true; c.manifest.requiresRecoveryPlan=true; c.manifest.capabilityScope={cs[i].cap};
        c.observation.present=true; c.observation.artifactVersion=p.releaseVersion; c.observation.architecture=trust::ArtifactArchitecture::X64;
        c.observation.observedSha256=c.manifest.expectedSha256; c.observation.signature=trust::SignatureState::ValidTrustedPublisher;
        c.observation.publisherIdentity=std::string(kPublisher);
        p.components.push_back(std::move(c));
    } return p;
}

trust::TrustPolicy policy() {
    trust::TrustPolicy p; p.hostArchitecture=trust::ArtifactArchitecture::X64;
    p.requireRedistributionPermission=true; p.allowedSourceIds={"hydraseat-release"};
    for(const auto& c:cs)p.allowedCapabilities.push_back(c.cap);
    p.trustedPublisherIdentities={std::string(kPublisher)};
    return p;
}

InstalledState installed(const InstallerPackage& p) {
    InstalledState s; s.installedVersion=p.releaseVersion; s.installedRevision=p.releaseRevision;
    s.uninstallRegistered=true;
    for(const auto& c:p.components)s.ownedComponents.push_back({c.component,c.manifest.artifactVersion,c.observation.observedSha256});
    return s;
}

ApplicationUpdateOffer offer(std::string version, std::uint64_t revision) {
    ApplicationUpdateOffer o; o.releaseNotesId="release-notes-"+std::to_string(revision);
    o.package=pkg(std::move(version),revision); o.restartRecommended=true; return o;
}

class Exec final : public InstallerExecutor {
public:
    std::optional<InstalledState> current;
    bool stageOk=true, commitOk=true, verifyOk=true, rollbackOk=true, verifyRollbackOk=true;
    int beginCalls=0, stageCalls=0, commitCalls=0, rollbackCalls=0;
    bool active=false;

    class Transaction final : public InstallerMutationTransaction {
    public:
        explicit Transaction(Exec& owner) : owner_(owner), previous_(owner.current) {}
        ~Transaction() override { owner_.active=false; }
        std::string_view transactionId() const noexcept override { return "update-transaction"; }
        const std::optional<InstalledState>& previousState() const noexcept override { return previous_; }
        bool stageInstallOrRepair(InstallerAction,const InstallerPackage& p,const InstallerOptions& o) noexcept override {
            ++owner_.stageCalls; if(!owner_.stageOk)return false;
            const bool profiles=previous_?previous_->userProfilesPresent:false;
            staged_=installed(p); staged_->startupMode=o.startupMode; staged_->userProfilesPresent=profiles; return true;
        }
        bool stageUninstall(const InstallerOptions&) noexcept override { return false; }
        bool commit() noexcept override {
            ++owner_.commitCalls; owner_.current=staged_; return owner_.commitOk;
        }
        bool verify(const std::optional<InstalledState>& expected) noexcept override {
            return owner_.verifyOk && owner_.current==expected;
        }
        bool rollback() noexcept override {
            ++owner_.rollbackCalls; if(!owner_.rollbackOk)return false; owner_.current=previous_; return true;
        }
        bool verifyRollback() noexcept override {
            return owner_.verifyRollbackOk && owner_.current==previous_;
        }
        bool finish(InstallerTransactionDisposition) noexcept override { return true; }
    private:
        Exec& owner_;
        std::optional<InstalledState> previous_;
        std::optional<InstalledState> staged_;
    };

    std::unique_ptr<InstallerMutationTransaction> beginMutation() noexcept override {
        ++beginCalls; if(active)return {}; active=true; return std::make_unique<Transaction>(*this);
    }
};

void testPreviewIsPureAndMonotonic() {
    auto cur=installed(pkg("1.0.0",100)); auto up=offer("1.1.0",110); UpdatePreview preview;
    check(previewApplicationUpdate(cur,up,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).succeeded() &&
          preview.currentRevision==100u && preview.targetRevision==110u && preview.approvalIdentity==makeApprovalIdentity(up),
          "newer trusted update produces deterministic pure preview");
    auto replay=offer("1.0.0",100);
    check(previewApplicationUpdate(cur,replay,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::NotNewer,
          "same-version/revision replay is rejected deterministically");
    auto older=offer("0.9.0",90);
    check(previewApplicationUpdate(cur,older,UpdateDirection::Rollback,InstallerEnvironment{},policy(),preview).succeeded(),
          "older trusted package may be previewed only as explicit rollback");
    check(previewApplicationUpdate(cur,up,UpdateDirection::Rollback,InstallerEnvironment{},policy(),preview).code==UpdateCode::InvalidRollbackTarget,
          "newer revision cannot masquerade as rollback");
}

void testApplyRequiresExactApprovalAndPreservesStartup() {
    auto cur=installed(pkg("1.0.0",100)); auto up=offer("1.1.0",110); Exec exec; exec.current=cur;
    InstallerReceipt receipt; InstallerOptions options;
    UpdateApproval noApproval{false,makeApprovalIdentity(up)};
    check(applyApplicationUpdate(cur,up,UpdateDirection::Upgrade,noApproval,InstallerEnvironment{},options,policy(),exec,receipt).code==UpdateCode::ApprovalRequired && exec.stageCalls==0,
          "executable update cannot apply silently without user approval");
    UpdateApproval wrong{true,"app-update:other"};
    check(applyApplicationUpdate(cur,up,UpdateDirection::Upgrade,wrong,InstallerEnvironment{},options,policy(),exec,receipt).code==UpdateCode::ApprovalMismatch && exec.stageCalls==0,
          "approval is bound to exact target version/revision/release notes identity");
    UpdateApproval ok{true,makeApprovalIdentity(up)};
    check(applyApplicationUpdate(cur,up,UpdateDirection::Upgrade,ok,InstallerEnvironment{},options,policy(),exec,receipt).succeeded() &&
          exec.current->installedRevision==110u && exec.current->startupMode==startup::StartupMode::Manual &&
          receipt.status==InstallerReceiptStatus::Applied,
          "approved update reuses the lock-owned CAS installer transaction and preserves startup mode");
}

void testTrustPublisherTamperAndFailureRollback() {
    auto cur=installed(pkg("1.0.0",100)); auto up=offer("1.1.0",110); UpdatePreview preview;
    up.package.components[0].observation.observedSha256=std::string(64u,'f');
    check(previewApplicationUpdate(cur,up,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::TrustRejected,
          "signature/hash mismatch is rejected during preview before mutation");

    auto wrongPublisher=offer("1.1.0",110);
    wrongPublisher.package.components[0].observation.publisherIdentity=std::string(kWrongPublisher);
    check(previewApplicationUpdate(cur,wrongPublisher,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::TrustRejected,
          "unexpected exact publisher is rejected even when signature status is valid");

    up=offer("1.1.0",110); Exec exec; exec.current=cur; exec.commitOk=false;
    InstallerReceipt receipt; InstallerOptions options; UpdateApproval ok{true,makeApprovalIdentity(up)};
    auto result=applyApplicationUpdate(cur,up,UpdateDirection::Upgrade,ok,InstallerEnvironment{},options,policy(),exec,receipt);
    check(result.code==UpdateCode::InstallerRejected && result.installerCode==InstallerCode::ApplyFailedRolledBack &&
          exec.current==cur && exec.rollbackCalls==1 && receipt.status==InstallerReceiptStatus::RolledBack,
          "failed approved update restores previous installed version through installer rollback");
}

void testExplicitRollbackUsesSameTrustAndApproval() {
    auto cur=installed(pkg("1.1.0",110)); auto old=offer("1.0.0",100); Exec exec; exec.current=cur;
    InstallerReceipt receipt; InstallerOptions options; UpdateApproval approval{true,makeApprovalIdentity(old)};
    check(applyApplicationUpdate(cur,old,UpdateDirection::Rollback,approval,InstallerEnvironment{},options,policy(),exec,receipt).succeeded() &&
          exec.current->installedRevision==100u,
          "rollback to an older signed package is explicit, trusted, approved, and transactional");
}

void testUpdateCannotChangeStartupPreference() {
    auto cur=installed(pkg("1.0.0",100)); cur.startupMode=startup::StartupMode::BackgroundIdle;
    auto up=offer("1.1.0",110); Exec exec; exec.current=cur;
    InstallerOptions options; options.startupMode=startup::StartupMode::Manual;
    InstallerReceipt receipt; UpdateApproval approval{true,makeApprovalIdentity(up)};
    check(applyApplicationUpdate(cur,up,UpdateDirection::Upgrade,approval,InstallerEnvironment{},options,policy(),exec,receipt).code==UpdateCode::InvalidOffer && exec.stageCalls==0,
          "software update cannot silently disable/change the user's chosen startup policy");
}

void testApprovalBindsExactTrustedPackageContentsAndPublisher() {
    auto cur=installed(pkg("1.0.0",100)); auto previewed=offer("1.1.0",110);
    const auto approvedIdentity=makeApprovalIdentity(previewed);
    auto replacement=previewed;
    replacement.package.components[0].manifest.expectedSha256=std::string(64u,'b');
    replacement.package.components[0].observation.observedSha256=std::string(64u,'b');
    check(makeApprovalIdentity(replacement)!=approvedIdentity,
          "approval identity changes when trusted package bytes change under reused release metadata");

    auto publisherReplacement=previewed;
    publisherReplacement.package.components[0].observation.publisherIdentity=std::string(kWrongPublisher);
    check(makeApprovalIdentity(publisherReplacement)!=approvedIdentity,
          "approval identity includes exact observed publisher identity rather than signed status only");

    Exec exec; exec.current=cur; InstallerReceipt receipt; InstallerOptions options;
    UpdateApproval staleApproval{true,approvedIdentity};
    const auto result=applyApplicationUpdate(cur,replacement,UpdateDirection::Upgrade,staleApproval,
                                             InstallerEnvironment{},options,policy(),exec,receipt);
    check(result.code==UpdateCode::ApprovalMismatch && exec.stageCalls==0,
          "approval for one trusted package cannot authorize different trusted bytes with reused metadata");
}

void testApplyRejectsChangedInstalledStateAndCompetingWriter() {
    auto previewCurrent=installed(pkg("1.0.0",100)); auto up=offer("1.1.0",110);
    Exec changed; changed.current=installed(pkg("1.2.0",120));
    InstallerReceipt receipt; InstallerOptions options; UpdateApproval approval{true,makeApprovalIdentity(up)};
    const auto result=applyApplicationUpdate(previewCurrent,up,UpdateDirection::Upgrade,approval,
                                             InstallerEnvironment{},options,policy(),changed,receipt);
    check(result.code==UpdateCode::StaleInstalledState && changed.stageCalls==0 && changed.rollbackCalls==0 &&
          changed.current && changed.current->installedRevision==120u && receipt.status==InstallerReceiptStatus::Rejected,
          "authoritative installed state is rechecked inside mutation boundary before staging");

    Exec competing; competing.current=previewCurrent;
    auto firstOffer=offer("1.1.0",110); auto secondOffer=offer("1.2.0",120);
    UpdateApproval firstApproval{true,makeApprovalIdentity(firstOffer)};
    UpdateApproval secondApproval{true,makeApprovalIdentity(secondOffer)};
    InstallerReceipt firstReceipt; InstallerReceipt secondReceipt;
    const auto first=applyApplicationUpdate(previewCurrent,firstOffer,UpdateDirection::Upgrade,firstApproval,
                                            InstallerEnvironment{},options,policy(),competing,firstReceipt);
    const auto second=applyApplicationUpdate(previewCurrent,secondOffer,UpdateDirection::Upgrade,secondApproval,
                                             InstallerEnvironment{},options,policy(),competing,secondReceipt);
    check(first.succeeded() && second.code==UpdateCode::StaleInstalledState && competing.current &&
          competing.current->installedRevision==110u && competing.stageCalls==1 && competing.commitCalls==1,
          "two competing updates cannot silently overwrite; first commit wins and stale writer fails closed");
}

void testPreviewRejectsMalformedOrUnboundedCurrentState() {
    auto up=offer("1.1.0",110); UpdatePreview preview;
    auto mixed=installed(pkg("1.0.0",100)); mixed.ownedComponents[0].version="0.9.0";
    check(previewApplicationUpdate(mixed,up,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::InvalidOffer,
          "update preview rejects mixed-version installed component state before approval");

    auto invalidStartup=installed(pkg("1.0.0",100)); invalidStartup.startupMode=static_cast<startup::StartupMode>(0xffu);
    check(previewApplicationUpdate(invalidStartup,up,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::InvalidOffer,
          "update preview rejects invalid persisted startup mode before approval");

    auto oversized=installed(pkg("1.0.0",100));
    while(oversized.ownedComponents.size()<=kMaximumInstallerComponents) {
        oversized.ownedComponents.push_back(oversized.ownedComponents.front());
    }
    check(previewApplicationUpdate(oversized,up,UpdateDirection::Upgrade,InstallerEnvironment{},policy(),preview).code==UpdateCode::InvalidOffer,
          "update preview rejects unbounded installed component state before canonical comparison");
}

} // namespace

int main() {
    testPreviewIsPureAndMonotonic();
    testApplyRequiresExactApprovalAndPreservesStartup();
    testTrustPublisherTamperAndFailureRollback();
    testExplicitRollbackUsesSameTrustAndApproval();
    testUpdateCannotChangeStartupPreference();
    testApprovalBindsExactTrustedPackageContentsAndPublisher();
    testApplyRejectsChangedInstalledStateAndCompetingWriter();
    testPreviewRejectsMalformedOrUnboundedCurrentState();
    if(failures){std::cerr<<failures<<" update transaction test(s) failed.\n";return EXIT_FAILURE;}
    std::cout<<"Update transaction tests passed.\n";
    return EXIT_SUCCESS;
}
