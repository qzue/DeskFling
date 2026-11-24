#pragma once
#import <Foundation/Foundation.h>
#include <memory>
#include <string>
// UI owns cached resources before start. Recovery remains in an independent process.
class DPPProtectedMirrorSession {
    struct Impl;
    std::unique_ptr<Impl> impl_;

   public:
    DPPProtectedMirrorSession();
    ~DPPProtectedMirrorSession();
    bool start(NSDictionary* finder, NSUInteger itemCount, NSString* snapshotID, bool continuous = false);
    bool tick();
    bool notifySystemSleep(bool sleeping);
    bool stop();
    unsigned long long remainingMs() const;
    bool isContinuous() const;
    std::string failure() const;
    NSDictionary* diagnosticIdentity() const;
    NSDictionary* sampleTiming() const;
    void drainTimingSamples() const;
};
int runProtectedMirrorPanel(bool checkOnly = false);
int auditProtectedMirrorScope();
int requestFinderAuthorizationFromUI();
bool recoverProtectedMirrorJournalFromUI();
NSString* dppAppLogDirectoryPath();
