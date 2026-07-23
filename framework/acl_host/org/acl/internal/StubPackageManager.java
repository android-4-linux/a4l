// SPDX-License-Identifier: Apache-2.0
// Provenance: GENERATED (tools/host_stubs/GenStub via reflection) — every abstract
// android.content.pm.PackageManager method defaulted to null/false/0. acl has no Binder/PMS
// (ADR-3), so there is no real PackageManager; app-meaningful methods are overridden in
// HostBootstrap.AppPackageManager. Regenerate against the framework classes.jar. No GPL-3.0 sources.
package org.acl.internal;
@SuppressWarnings({"unchecked","rawtypes","deprecation"})
public class StubPackageManager extends android.content.pm.PackageManager {
  // NOTE: the query*/getInstalled*/getShared* APIs return an EMPTY list when nothing
  // matches -- that is the platform contract, and callers iterate the result without a
  // null check (androidx MediaButtonReceiver.getMediaButtonReceiverComponent does
  // queryBroadcastReceivers(...).size(), which NPEs on null and takes MediaSessionCompat
  // -- and so every media app -- down with it). Single-object lookups (resolveActivity,
  // resolveService, getPackageInfo) DO return null/throw when absent, matching the
  // platform.
  public android.content.pm.PackageInfo getPackageInfo(android.content.pm.VersionedPackage p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.PackageInfo getPackageInfo(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public int checkPermission(java.lang.String p0, java.lang.String p1) { return 0; }
  public android.content.pm.PackageInfo getPackageInfoAsUser(java.lang.String p0, int p1, int p2) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public java.lang.String[] currentToCanonicalPackageNames(java.lang.String[] p0) { return null; }
  public java.lang.String[] canonicalToCurrentPackageNames(java.lang.String[] p0) { return null; }
  public android.content.Intent getLaunchIntentForPackage(java.lang.String p0) { return null; }
  public android.content.Intent getLeanbackLaunchIntentForPackage(java.lang.String p0) { return null; }
  public android.content.Intent getCarLaunchIntentForPackage(java.lang.String p0) { return null; }
  public int[] getPackageGids(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public int[] getPackageGids(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public int getPackageUid(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public int getPackageUidAsUser(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public int getPackageUidAsUser(java.lang.String p0, int p1, int p2) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public android.content.pm.PermissionInfo getPermissionInfo(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public java.util.List queryPermissionsByGroup(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public boolean arePermissionsIndividuallyControlled() { return false; }
  public boolean isWirelessConsentModeEnabled() { return false; }
  public android.content.pm.PermissionGroupInfo getPermissionGroupInfo(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public java.util.List getAllPermissionGroups(int p0) { return java.util.Collections.emptyList(); }
  public android.content.pm.ApplicationInfo getApplicationInfo(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.ApplicationInfo getApplicationInfoAsUser(java.lang.String p0, int p1, int p2) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.ActivityInfo getActivityInfo(android.content.ComponentName p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.ActivityInfo getReceiverInfo(android.content.ComponentName p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.ServiceInfo getServiceInfo(android.content.ComponentName p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.pm.ProviderInfo getProviderInfo(android.content.ComponentName p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public java.util.List getInstalledPackages(int p0) { return java.util.Collections.emptyList(); }
  public java.util.List getPackagesHoldingPermissions(java.lang.String[] p0, int p1) { return java.util.Collections.emptyList(); }
  public java.util.List getInstalledPackagesAsUser(int p0, int p1) { return java.util.Collections.emptyList(); }
  public boolean isPermissionRevokedByPolicy(java.lang.String p0, java.lang.String p1) { return false; }
  public boolean addPermission(android.content.pm.PermissionInfo p0) { return false; }
  public boolean addPermissionAsync(android.content.pm.PermissionInfo p0) { return false; }
  public void removePermission(java.lang.String p0) {  }
  public void grantRuntimePermission(java.lang.String p0, java.lang.String p1, android.os.UserHandle p2) {  }
  public void revokeRuntimePermission(java.lang.String p0, java.lang.String p1, android.os.UserHandle p2) {  }
  public int getPermissionFlags(java.lang.String p0, java.lang.String p1, android.os.UserHandle p2) { return 0; }
  public void updatePermissionFlags(java.lang.String p0, java.lang.String p1, int p2, int p3, android.os.UserHandle p4) {  }
  public boolean shouldShowRequestPermissionRationale(java.lang.String p0) { return false; }
  public int checkSignatures(java.lang.String p0, java.lang.String p1) { return 0; }
  public int checkSignatures(int p0, int p1) { return 0; }
  public java.lang.String[] getPackagesForUid(int p0) { return null; }
  public java.lang.String getNameForUid(int p0) { return null; }
  public java.lang.String[] getNamesForUids(int[] p0) { return null; }
  public int getUidForSharedUser(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public java.util.List getInstalledApplications(int p0) { return java.util.Collections.emptyList(); }
  public java.util.List getInstalledApplicationsAsUser(int p0, int p1) { return java.util.Collections.emptyList(); }
  public java.util.List getInstantApps() { return java.util.Collections.emptyList(); }
  public android.graphics.drawable.Drawable getInstantAppIcon(java.lang.String p0) { return null; }
  public boolean isInstantApp() { return false; }
  public boolean isInstantApp(java.lang.String p0) { return false; }
  public int getInstantAppCookieMaxBytes() { return 0; }
  public int getInstantAppCookieMaxSize() { return 0; }
  public byte[] getInstantAppCookie() { return null; }
  public void clearInstantAppCookie() {  }
  public void updateInstantAppCookie(byte[] p0) {  }
  public boolean setInstantAppCookie(byte[] p0) { return false; }
  public java.lang.String[] getSystemSharedLibraryNames() { return null; }
  public java.util.List getSharedLibraries(int p0) { return java.util.Collections.emptyList(); }
  public java.util.List getSharedLibrariesAsUser(int p0, int p1) { return java.util.Collections.emptyList(); }
  public java.lang.String getServicesSystemSharedLibraryPackageName() { return null; }
  public java.lang.String getSharedSystemSharedLibraryPackageName() { return null; }
  public android.content.pm.ChangedPackages getChangedPackages(int p0) { return null; }
  public android.content.pm.FeatureInfo[] getSystemAvailableFeatures() { return null; }
  public boolean hasSystemFeature(java.lang.String p0) { return false; }
  public boolean hasSystemFeature(java.lang.String p0, int p1) { return false; }
  public android.content.pm.ResolveInfo resolveActivity(android.content.Intent p0, int p1) { return null; }
  public android.content.pm.ResolveInfo resolveActivityAsUser(android.content.Intent p0, int p1, int p2) { return null; }
  public java.util.List queryIntentActivities(android.content.Intent p0, int p1) { return java.util.Collections.emptyList(); }
  public java.util.List queryIntentActivitiesAsUser(android.content.Intent p0, int p1, int p2) { return java.util.Collections.emptyList(); }
  public java.util.List queryIntentActivityOptions(android.content.ComponentName p0, android.content.Intent[] p1, android.content.Intent p2, int p3) { return java.util.Collections.emptyList(); }
  public java.util.List queryBroadcastReceivers(android.content.Intent p0, int p1) { return java.util.Collections.emptyList(); }
  public java.util.List queryBroadcastReceiversAsUser(android.content.Intent p0, int p1, int p2) { return java.util.Collections.emptyList(); }
  public android.content.pm.ResolveInfo resolveService(android.content.Intent p0, int p1) { return null; }
  public android.content.pm.ResolveInfo resolveServiceAsUser(android.content.Intent p0, int p1, int p2) { return null; }
  public java.util.List queryIntentServices(android.content.Intent p0, int p1) { return java.util.Collections.emptyList(); }
  public java.util.List queryIntentServicesAsUser(android.content.Intent p0, int p1, int p2) { return java.util.Collections.emptyList(); }
  public java.util.List queryIntentContentProvidersAsUser(android.content.Intent p0, int p1, int p2) { return java.util.Collections.emptyList(); }
  public java.util.List queryIntentContentProviders(android.content.Intent p0, int p1) { return java.util.Collections.emptyList(); }
  public android.content.pm.ProviderInfo resolveContentProvider(java.lang.String p0, int p1) { return null; }
  public android.content.pm.ProviderInfo resolveContentProviderAsUser(java.lang.String p0, int p1, int p2) { return null; }
  public java.util.List queryContentProviders(java.lang.String p0, int p1, int p2) { return java.util.Collections.emptyList(); }
  public android.content.pm.InstrumentationInfo getInstrumentationInfo(android.content.ComponentName p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public java.util.List queryInstrumentation(java.lang.String p0, int p1) { return java.util.Collections.emptyList(); }
  public android.graphics.drawable.Drawable getDrawable(java.lang.String p0, int p1, android.content.pm.ApplicationInfo p2) { return null; }
  public android.graphics.drawable.Drawable getActivityIcon(android.content.ComponentName p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getActivityIcon(android.content.Intent p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getActivityBanner(android.content.ComponentName p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getActivityBanner(android.content.Intent p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getDefaultActivityIcon() { return null; }
  public android.graphics.drawable.Drawable getApplicationIcon(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getApplicationIcon(android.content.pm.ApplicationInfo p0) { return null; }
  public android.graphics.drawable.Drawable getApplicationBanner(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getApplicationBanner(android.content.pm.ApplicationInfo p0) { return null; }
  public android.graphics.drawable.Drawable getActivityLogo(android.content.ComponentName p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getActivityLogo(android.content.Intent p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getApplicationLogo(android.content.pm.ApplicationInfo p0) { return null; }
  public android.graphics.drawable.Drawable getApplicationLogo(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.graphics.drawable.Drawable getUserBadgedIcon(android.graphics.drawable.Drawable p0, android.os.UserHandle p1) { return null; }
  public android.graphics.drawable.Drawable getUserBadgedDrawableForDensity(android.graphics.drawable.Drawable p0, android.os.UserHandle p1, android.graphics.Rect p2, int p3) { return null; }
  public android.graphics.drawable.Drawable getUserBadgeForDensity(android.os.UserHandle p0, int p1) { return null; }
  public android.graphics.drawable.Drawable getUserBadgeForDensityNoBackground(android.os.UserHandle p0, int p1) { return null; }
  public java.lang.CharSequence getUserBadgedLabel(java.lang.CharSequence p0, android.os.UserHandle p1) { return null; }
  public java.lang.CharSequence getText(java.lang.String p0, int p1, android.content.pm.ApplicationInfo p2) { return null; }
  public android.content.res.XmlResourceParser getXml(java.lang.String p0, int p1, android.content.pm.ApplicationInfo p2) { return null; }
  public java.lang.CharSequence getApplicationLabel(android.content.pm.ApplicationInfo p0) { return null; }
  public android.content.res.Resources getResourcesForActivity(android.content.ComponentName p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.res.Resources getResourcesForApplication(android.content.pm.ApplicationInfo p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.res.Resources getResourcesForApplication(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public android.content.res.Resources getResourcesForApplicationAsUser(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return null; }
  public int installExistingPackage(java.lang.String p0) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public int installExistingPackage(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public int installExistingPackageAsUser(java.lang.String p0, int p1) throws android.content.pm.PackageManager.NameNotFoundException { return 0; }
  public void verifyPendingInstall(int p0, int p1) {  }
  public void extendVerificationTimeout(int p0, int p1, long p2) {  }
  public void verifyIntentFilter(int p0, int p1, java.util.List p2) {  }
  public int getIntentVerificationStatusAsUser(java.lang.String p0, int p1) { return 0; }
  public boolean updateIntentVerificationStatusAsUser(java.lang.String p0, int p1, int p2) { return false; }
  public java.util.List getIntentFilterVerifications(java.lang.String p0) { return java.util.Collections.emptyList(); }
  public java.util.List getAllIntentFilters(java.lang.String p0) { return java.util.Collections.emptyList(); }
  public java.lang.String getDefaultBrowserPackageNameAsUser(int p0) { return null; }
  public boolean setDefaultBrowserPackageNameAsUser(java.lang.String p0, int p1) { return false; }
  public void setInstallerPackageName(java.lang.String p0, java.lang.String p1) {  }
  public void setUpdateAvailable(java.lang.String p0, boolean p1) {  }
  public void deletePackage(java.lang.String p0, android.content.pm.IPackageDeleteObserver p1, int p2) {  }
  public void deletePackageAsUser(java.lang.String p0, android.content.pm.IPackageDeleteObserver p1, int p2, int p3) {  }
  public java.lang.String getInstallerPackageName(java.lang.String p0) { return null; }
  public void clearApplicationUserData(java.lang.String p0, android.content.pm.IPackageDataObserver p1) {  }
  public void deleteApplicationCacheFiles(java.lang.String p0, android.content.pm.IPackageDataObserver p1) {  }
  public void deleteApplicationCacheFilesAsUser(java.lang.String p0, int p1, android.content.pm.IPackageDataObserver p2) {  }
  public void freeStorageAndNotify(java.lang.String p0, long p1, android.content.pm.IPackageDataObserver p2) {  }
  public void freeStorage(java.lang.String p0, long p1, android.content.IntentSender p2) {  }
  public void getPackageSizeInfoAsUser(java.lang.String p0, int p1, android.content.pm.IPackageStatsObserver p2) {  }
  public void addPackageToPreferred(java.lang.String p0) {  }
  public void removePackageFromPreferred(java.lang.String p0) {  }
  public java.util.List getPreferredPackages(int p0) { return java.util.Collections.emptyList(); }
  public void addPreferredActivity(android.content.IntentFilter p0, int p1, android.content.ComponentName[] p2, android.content.ComponentName p3) {  }
  public void replacePreferredActivity(android.content.IntentFilter p0, int p1, android.content.ComponentName[] p2, android.content.ComponentName p3) {  }
  public void clearPackagePreferredActivities(java.lang.String p0) {  }
  public int getPreferredActivities(java.util.List p0, java.util.List p1, java.lang.String p2) { return 0; }
  public android.content.ComponentName getHomeActivities(java.util.List p0) { return null; }
  public void setComponentEnabledSetting(android.content.ComponentName p0, int p1, int p2) {  }
  public int getComponentEnabledSetting(android.content.ComponentName p0) { return 0; }
  public void setApplicationEnabledSetting(java.lang.String p0, int p1, int p2) {  }
  public int getApplicationEnabledSetting(java.lang.String p0) { return 0; }
  public void flushPackageRestrictionsAsUser(int p0) {  }
  public boolean setApplicationHiddenSettingAsUser(java.lang.String p0, boolean p1, android.os.UserHandle p2) { return false; }
  public boolean getApplicationHiddenSettingAsUser(java.lang.String p0, android.os.UserHandle p1) { return false; }
  public boolean isSafeMode() { return false; }
  public void addOnPermissionsChangeListener(android.content.pm.PackageManager.OnPermissionsChangedListener p0) {  }
  public void removeOnPermissionsChangeListener(android.content.pm.PackageManager.OnPermissionsChangedListener p0) {  }
  public android.content.pm.KeySet getKeySetByAlias(java.lang.String p0, java.lang.String p1) { return null; }
  public android.content.pm.KeySet getSigningKeySet(java.lang.String p0) { return null; }
  public boolean isSignedBy(java.lang.String p0, android.content.pm.KeySet p1) { return false; }
  public boolean isSignedByExactly(java.lang.String p0, android.content.pm.KeySet p1) { return false; }
  public boolean isPackageSuspendedForUser(java.lang.String p0, int p1) { return false; }
  public void setApplicationCategoryHint(java.lang.String p0, int p1) {  }
  public int getMoveStatus(int p0) { return 0; }
  public void registerMoveCallback(android.content.pm.PackageManager.MoveCallback p0, android.os.Handler p1) {  }
  public void unregisterMoveCallback(android.content.pm.PackageManager.MoveCallback p0) {  }
  public int movePackage(java.lang.String p0, android.os.storage.VolumeInfo p1) { return 0; }
  public android.os.storage.VolumeInfo getPackageCurrentVolume(android.content.pm.ApplicationInfo p0) { return null; }
  public java.util.List getPackageCandidateVolumes(android.content.pm.ApplicationInfo p0) { return java.util.Collections.emptyList(); }
  public int movePrimaryStorage(android.os.storage.VolumeInfo p0) { return 0; }
  public android.os.storage.VolumeInfo getPrimaryStorageCurrentVolume() { return null; }
  public java.util.List getPrimaryStorageCandidateVolumes() { return java.util.Collections.emptyList(); }
  public android.content.pm.VerifierDeviceIdentity getVerifierDeviceIdentity() { return null; }
  public boolean isUpgrade() { return false; }
  public android.content.pm.PackageInstaller getPackageInstaller() { return null; }
  public void addCrossProfileIntentFilter(android.content.IntentFilter p0, int p1, int p2, int p3) {  }
  public void clearCrossProfileIntentFilters(int p0) {  }
  public android.graphics.drawable.Drawable loadItemIcon(android.content.pm.PackageItemInfo p0, android.content.pm.ApplicationInfo p1) { return null; }
  public android.graphics.drawable.Drawable loadUnbadgedItemIcon(android.content.pm.PackageItemInfo p0, android.content.pm.ApplicationInfo p1) { return null; }
  public boolean isPackageAvailable(java.lang.String p0) { return false; }
  public int getInstallReason(java.lang.String p0, android.os.UserHandle p1) { return 0; }
  public boolean canRequestPackageInstalls() { return false; }
  public android.content.ComponentName getInstantAppResolverSettingsComponent() { return null; }
  public android.content.ComponentName getInstantAppInstallerComponent() { return null; }
  public java.lang.String getInstantAppAndroidId(java.lang.String p0, android.os.UserHandle p1) { return null; }
  public void registerDexModule(java.lang.String p0, android.content.pm.PackageManager.DexModuleRegisterCallback p1) {  }
}
