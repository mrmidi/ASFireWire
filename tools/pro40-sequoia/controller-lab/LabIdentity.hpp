// SPDX-License-Identifier: Apache-2.0
#pragma once
// One identity per binary. Legacy is retained for existing journal recovery;
// the signed namespace must never admit legacy service records as a fallback.
#if defined(PRO40_LAB_SESSION_NAMESPACE) && defined(PRO40_LAB_DUPLEX_HARDWARE_NAMESPACE)
#define PRO40_LAB_APP_ID "local.pro40.session-controller"
#define PRO40_LAB_FIRST_USE_REVIEW "Normal-session development identity; require fresh independent BTM absence and empty journal review before first registration"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-SessionController"
#elif defined(PRO40_LAB_SESSION_NAMESPACE) && defined(PRO40_LAB_DUPLEX_NAMESPACE)
#define PRO40_LAB_APP_ID "local.pro40.session-fixture-controller"
#define PRO40_LAB_FIRST_USE_REVIEW "Generated session identity; require fresh independent BTM absence and empty journal review before first registration"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-SessionFixtureController"
#elif defined(PRO40_LAB_DUPLEX_HARDWARE_NAMESPACE)
#define PRO40_LAB_APP_ID "local.pro40.duplex-hardware-lab"
#define PRO40_LAB_FIRST_USE_REVIEW "Isolated hardware duplex identity; require fresh independent BTM absence and empty journal review before first registration"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-DuplexHardwareLab"
#elif defined(PRO40_LAB_DUPLEX_NAMESPACE)
#define PRO40_LAB_APP_ID "local.pro40.duplex-controller-lab"
#define PRO40_LAB_FIRST_USE_REVIEW "Isolated duplex lab identity; require fresh independent BTM absence and empty journal review before first registration"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-DuplexControllerLab"
#elif defined(PRO40_LAB_SIGNED_NAMESPACE)
#define PRO40_LAB_APP_ID "local.pro40.signed-controller-lab"
#define PRO40_LAB_FIRST_USE_REVIEW "Lab identity introduced in 79cebf15; signed namespace requires fresh independent BTM absence and empty journal review before first registration"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-SignedControllerLab"
#else
#define PRO40_LAB_APP_ID "local.pro40.controller-lab"
#define PRO40_LAB_FIRST_USE_REVIEW "Lab history review b933122b: this fixed agent identity has not been registered by this project; retain fresh independent absence and journal checks"
#define PRO40_LAB_JOURNAL "Library/Application Support/ASFW-Pro40-ControllerLab"
#endif
#define PRO40_LAB_SERVICE_ID PRO40_LAB_APP_ID ".verifier"
#define PRO40_LAB_PLIST PRO40_LAB_SERVICE_ID ".plist"
#define PRO40_LAB_PLIST_SUFFIX "/Contents/Library/LaunchAgents/" PRO40_LAB_PLIST
