#import <Foundation/Foundation.h>

#import "YapDatabaseCore.h"
#import "YapDatabaseConnection.h"
#import "YapDatabaseConnectionConfig.h"
#import "YapDatabaseTransaction.h"
#import "YapDatabaseExtension.h"

#import "YapBidirectionalCache.h"
#import "YapCache.h"
#import "YapCollectionKey.h"
#import "YapDatabaseCollectionConfig.h"
#import "YapMemoryTable.h"
#import "YapMutationStack.h"

#import <SQLCipher/sqlite3.h>
#import "yap_vfs_shim.h"

NS_ASSUME_NONNULL_BEGIN

/**
 * Helper method to conditionally invoke sqlite3_finalize on a statement, and then set the ivar to NULL.
 */
NS_INLINE void sqlite_finalize_null(sqlite3_stmt *_Nonnull* _Nonnull stmtPtr)
{
	if (stmtPtr && *stmtPtr)
	{
		sqlite3_finalize(*stmtPtr);
		*stmtPtr = NULL;
	}
}

NS_INLINE void sqlite_enum_reset(sqlite3_stmt *stmt, BOOL needsFinalize)
{
	if (stmt)
	{
		sqlite3_clear_bindings(stmt);
		sqlite3_reset(stmt);
		
		if (needsFinalize) {
			sqlite3_finalize(stmt);
		}
	}
}

#ifndef SQLITE_BIND_START
#define SQLITE_BIND_START 1
#endif

#ifndef SQLITE_COLUMN_START
#define SQLITE_COLUMN_START 0
#endif

/**
 * Keys for changeset dictionary.
 */

extern NSString *const YapDatabaseRegisteredExtensionsKey;
extern NSString *const YapDatabaseRegisteredMemoryTablesKey;
extern NSString *const YapDatabaseExtensionsOrderKey;
extern NSString *const YapDatabaseExtensionDependenciesKey;
extern NSString *const YapDatabaseRemovedRowidsKey;
extern NSString *const YapDatabaseNotificationKey;

/**
 * Key(s) for yap2 extension configuration table.
 *
 * This is the only key that is reserved, and should not be set by extension subclasses.
 */
static NSString *const ext_key_class = @"class";


@interface YapDatabase () {
@public
	
	NSString *yap_vfs_shim_name;
	yap_vfs *yap_vfs_shim;
	
	void *IsOnSnapshotQueueKey;       // Only to be used by YapDatabaseConnection
	void *IsOnWriteQueueKey;          // Only to be used by YapDatabaseConnection
	
	dispatch_queue_t snapshotQueue;   // Only to be used by YapDatabaseConnection
	dispatch_queue_t writeQueue;      // Only to be used by YapDatabaseConnection
	
	NSMutableArray *connectionStates; // Only to be used by YapDatabaseConnection
	
	NSArray *previouslyRegisteredExtensionNames; // Writeable only within snapshot queue
}

/**
 * General utility methods.
 */

+ (int64_t)pragma:(NSString *)pragmaSetting using:(sqlite3 *)aDb;

+ (NSString *)pragmaValueForSynchronous:(int64_t)synchronous;
+ (NSString *)pragmaValueForAutoVacuum:(int64_t)auto_vacuum;


+ (BOOL)tableExists:(NSString *)tableName using:(sqlite3 *)aDb;
+ (NSArray *)tableNamesUsing:(sqlite3 *)aDb;
+ (NSArray *)columnNamesForTable:(NSString *)tableName using:(sqlite3 *)aDb;
+ (NSDictionary *)columnNamesAndAffinityForTable:(NSString *)tableName using:(sqlite3 *)aDb;

/**
 * Called from YapDatabaseConnection's dealloc method to remove connection's state from connectionStates array.
 */
- (void)removeConnection:(YapDatabaseConnection *)connection;

/**
 * YapDatabaseConnection uses these methods to recycle sqlite3 instances using the connection pool.
 */
- (BOOL)connectionPoolEnqueue:(sqlite3 *)aDb main_file:(yap_file *)main_file wal_file:(yap_file *)wal_file;
- (BOOL)connectionPoolDequeue:(sqlite3 *_Nonnull*_Nonnull)aDb main_file:(yap_file *_Nonnull*_Nonnull)main_file wal_file:(yap_file *_Nonnull*_Nonnull)wal_file;

- (YapDatabaseDeserializer)objectDeserializerForCollection:(nullable NSString *)collection;
- (YapDatabaseDeserializer)metadataDeserializerForCollection:(nullable NSString *)collection;

- (YapDatabaseCollectionConfig *)configForCollection:(nullable NSString *)collection;

- (NSNumber *)getDefaultObjectPolicy;
- (NSNumber *)getDefaultMetadataPolicy;
- (void)getObjectPolicies:(NSDictionary<NSString*, NSNumber*> *_Nonnull *_Nonnull)objectPoliciesPtr
         metadataPolicies:(NSDictionary<NSString*, NSNumber*> *_Nonnull *_Nonnull)metadataPoliciesPtr;

/**
 * These methods are only accessible from within the snapshotQueue.
 * Used by [YapDatabaseConnection prepare].
 */
- (NSDictionary *)registeredMemoryTables;
- (NSArray *)extensionsOrder;
- (NSDictionary *)extensionDependencies;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * Prior to starting the sqlite commit, the connection must report its changeset to the database.
 * The database will store the changeset, and provide it to other connections if needed (due to a race condition).
 * 
 * The following MUST be in the dictionary:
 *
 * - snapshot : NSNumber with the changeset's snapshot
 */
- (void)notePendingChangeset:(NSDictionary *)changeset fromConnection:(YapDatabaseConnection *)connection;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * This method is used if a transaction finds itself in a race condition.
 * That is, the transaction started before it was able to process changesets from sibling connections.
 * 
 * It should fetch the changesets needed and then process them via [connection noteCommittedChangeset:].
 */
- (NSArray *)pendingAndCommittedChangesetsSince:(uint64_t)connectionSnapshot until:(uint64_t)maxSnapshot;

/**
 * This method is only accessible from within the snapshotQueue.
 * 
 * Upon completion of a readwrite transaction, the connection must report its changeset to the database.
 * The database will then forward the changeset to all other connections.
 * 
 * The following MUST be in the dictionary:
 * 
 * - snapshot : NSNumber with the changeset's snapshot
 */
- (void)noteCommittedChangeset:(NSDictionary *)changeset fromConnection:(YapDatabaseConnection *)connection;

/**
 * This method should be called whenever the maximum checkpointable snapshot is incremented.
 * 
 * A commit/snapshot cannot be checkpointed until every connection is at or past that commit/snapshot.
 * Luckily the state of every connection is known to the system.
 * Thus we know the point at which a commit/snapshot becomes checkpointable,
 * and we can thus optimize the checkpoint invocations such that
 * each invocation is able to checkpoint one or more commits.
 */
- (void)asyncCheckpoint:(uint64_t)maxCheckpointableSnapshot;

/**
 * When aggressive checkpointing is enabled (occurs automatically if the WAL grows too big),
 * then read-write transactions will automatically start performing checkpoint operations after each commit.
 */
- (BOOL)aggressiveCheckpointEnabled;
- (void)noteCheckpointWithTotalFrames:(int)totalFrameCount checkpointedFrames:(int)checkpointedFrameCount;

/**
 * Configures database encryption via SQLCipher.
  */
- (BOOL)configureEncryptionForDatabase:(sqlite3 *)sqlite;

@end

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma mark -
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

@interface YapDatabaseConnection () {	
@public
	__strong YapDatabase *database;
	
	sqlite3 *db;
	
	yap_file *main_file;
	yap_file *wal_file;
	
	dispatch_queue_t connectionQueue;     // For YapDatabaseExtensionConnection subclasses
	void *IsOnConnectionQueueKey;         // For YapDatabaseExtensionConnection subclasses
	
	NSArray *extensionsOrder;             // Read-only by YapDatabaseTransaction
	NSDictionary *extensionDependencies;  // Read-only for YapDatabaseExtensionTransaction subclasses
	
	BOOL hasDiskChanges;
	BOOL enableMultiProcessSupport;
	
	YapBidirectionalCache<NSNumber *, YapCollectionKey *> *keyCache;
	YapCache<YapCollectionKey *, id> *objectCache;
	YapCache<YapCollectionKey *, id> *metadataCache;
	
	NSUInteger objectCacheLimit;          // Read-only by transaction. Use as consideration of whether to add to cache.
	NSUInteger metadataCacheLimit;        // Read-only by transaction. Use as consideration of whether to add to cache.
	
	BOOL needsMarkSqlLevelSharedReadLock; // Read-only by transaction. Use as consideration of whether to invoke method.
	
	NSMutableDictionary *objectChanges;
	NSMutableDictionary *metadataChanges;
	NSMutableSet *insertedKeys;
	NSMutableSet *removedKeys;
	NSMutableSet *removedCollections;
	NSMutableSet *removedRowids;
	BOOL allKeysRemoved;
	BOOL externallyModified;
	
	YapMutationStack_Bool *mutationStack;
}

- (instancetype)initWithDatabase:(YapDatabase *)database;
- (instancetype)initWithDatabase:(YapDatabase *)database config:(nullable YapDatabaseConnectionConfig *)config;

- (sqlite3_stmt *)beginTransactionStatement;
- (sqlite3_stmt *)beginImmediateTransactionStatement;
- (sqlite3_stmt *)commitTransactionStatement;
- (sqlite3_stmt *)rollbackTransactionStatement;

- (sqlite3_stmt *)yapGetDataForKeyStatement;   // Against "yap" database, for internal use
- (sqlite3_stmt *)yapSetDataForKeyStatement;   // Against "yap" database, for internal use
- (sqlite3_stmt *)yapRemoveForKeyStatement;    // Against "yap" database, for internal use
- (sqlite3_stmt *)yapRemoveExtensionStatement; // Against "yap" database, for internal use

- (sqlite3_stmt *)getCollectionCountStatement;
- (sqlite3_stmt *)getKeyCountForCollectionStatement;
- (sqlite3_stmt *)getKeyCountForAllStatement;
- (sqlite3_stmt *)getCountForRowidStatement;
- (sqlite3_stmt *)getRowidForKeyStatement;
- (sqlite3_stmt *)getKeyForRowidStatement;
- (sqlite3_stmt *)getDataForRowidStatement;
- (sqlite3_stmt *)getMetadataForRowidStatement;
- (sqlite3_stmt *)getAllForRowidStatement;
- (sqlite3_stmt *)getDataForKeyStatement;
- (sqlite3_stmt *)getMetadataForKeyStatement;
- (sqlite3_stmt *)getAllForKeyStatement;
- (sqlite3_stmt *)insertForRowidStatement;
- (sqlite3_stmt *)updateAllForRowidStatement;
- (sqlite3_stmt *)updateObjectForRowidStatement;
- (sqlite3_stmt *)updateMetadataForRowidStatement;
- (sqlite3_stmt *)removeForRowidStatement;
- (sqlite3_stmt *)removeCollectionStatement;
- (sqlite3_stmt *)removeAllStatement;

- (sqlite3_stmt *)enumerateCollectionsStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateCollectionsForKeyStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysInCollectionStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysInAllCollectionsStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysAndMetadataInCollectionStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysAndMetadataInAllCollectionsStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysAndObjectsInCollectionStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateKeysAndObjectsInAllCollectionsStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateRowsInCollectionStatement:(BOOL *)needsFinalizePtr;
- (sqlite3_stmt *)enumerateRowsInAllCollectionsStatement:(BOOL *)needsFinalizePtr;

- (void)prepare;

- (YapDatabaseConnectionConfig *)copyConfig;
- (void)applyConfig:(YapDatabaseConnectionConfig *)config;

- (NSDictionary *)extensions;

- (BOOL)registerExtension:(YapDatabaseExtension *)extension withName:(NSString *)extensionName;
- (void)unregisterExtensionWithName:(NSString *)extensionName;

- (NSDictionary *)registeredMemoryTables;

- (BOOL)registerMemoryTable:(YapMemoryTable *)table withName:(NSString *)name;
- (void)unregisterMemoryTableWithName:(NSString *)name;

- (void)markSqlLevelSharedReadLockAcquired;

- (void)getInternalChangeset:(NSMutableDictionary *_Nonnull*_Nonnull)internalPtr
           externalChangeset:(NSMutableDictionary *_Nonnull*_Nonnull)externalPtr;

- (void)noteCommittedChangeset:(NSDictionary *)changeset;

- (BOOL)resetLongLivedReadTransaction;

@end

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma mark -
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

@interface YapDatabaseReadTransaction () {
@private
	NSMutableArray *orderedExtensions;
	BOOL extensionsReady;
	
	YapMemoryTableTransaction *yapMemoryTableTransaction;
	
@protected
	NSMutableDictionary *extensions;
	
@public
	__unsafe_unretained YapDatabaseConnection *connection;
	
	BOOL isReadWriteTransaction;
}

- (id)initWithConnection:(YapDatabaseConnection *)connection isReadWriteTransaction:(BOOL)flag;

- (void)beginTransaction;
- (void)beginImmediateTransaction;
- (void)preCommitReadWriteTransaction;
- (void)commitTransaction;
- (void)rollbackTransaction;

- (NSDictionary *)extensions;
- (NSArray *)orderedExtensions;

- (YapMemoryTableTransaction *)memoryTableTransaction:(NSString *)tableName;
- (YapMemoryTableTransaction *)yapMemoryTableTransaction;

- (BOOL)getBoolValue:(BOOL *)valuePtr forKey:(NSString *)key extension:(NSString *)extension;
- (BOOL)getIntValue:(int *)valuePtr forKey:(NSString *)key extension:(NSString *)extensionName;
- (BOOL)getDoubleValue:(double *)valuePtr forKey:(NSString *)key extension:(NSString *)extensionName;
- (NSString *)stringValueForKey:(NSString *)key extension:(NSString *)extensionName;
- (NSData *)dataValueForKey:(NSString *)key extension:(NSString *)extensionName;

- (NSException *)mutationDuringEnumerationException;

- (BOOL)getRowid:(int64_t *_Nullable)rowidPtr forCollectionKey:(YapCollectionKey *)collectionKey;
- (BOOL)getRowid:(int64_t *_Nullable)rowidPtr forKey:(NSString *)key inCollection:(NSString *)collection;

- (YapCollectionKey *)collectionKeyForRowid:(int64_t)rowid;

- (BOOL)getCollectionKey:(YapCollectionKey *_Nullable*_Nullable)collectionKeyPtr
                  object:(id _Nullable *_Nullable)objectPtr
                forRowid:(int64_t)rowid;

- (BOOL)getCollectionKey:(YapCollectionKey *_Nullable*_Nullable)collectionKeyPtr
                metadata:(id _Nullable *_Nullable)metadataPtr
                forRowid:(int64_t)rowid;

- (BOOL)getCollectionKey:(YapCollectionKey *_Nullable*_Nullable)collectionKeyPtr
						object:(id _Nullable *_Nullable)objectPtr
					 metadata:(id _Nullable *_Nullable)metadataPtr
				forRowid:(int64_t)rowid;

- (BOOL)hasRowid:(int64_t)rowid;

- (id)objectForKey:(NSString *)key inCollection:(NSString *)collection withRowid:(int64_t)rowid;
- (id)objectForCollectionKey:(YapCollectionKey *)cacheKey withRowid:(int64_t)rowid;

- (id)metadataForKey:(NSString *)key inCollection:(NSString *)collection withRowid:(int64_t)rowid;
- (id)metadataForCollectionKey:(YapCollectionKey *)cacheKey withRowid:(int64_t)rowid;

- (BOOL)getObject:(id _Nullable *_Nullable)objectPtr
			metadata:(id _Nullable *_Nullable)metadataPtr
           forKey:(NSString *)key
     inCollection:(NSString *)collection
        withRowid:(int64_t)rowid;

- (BOOL)getObject:(id _Nullable *_Nullable)objectPtr
			metadata:(id _Nullable *_Nullable)metadataPtr
 forCollectionKey:(YapCollectionKey *)collectionKey
        withRowid:(int64_t)rowid;

- (void)_enumerateKeysInCollection:(NSString *)collection
                        usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, BOOL *stop))block;

- (void)_enumerateKeysInCollections:(NSArray<NSString*> *)collections
                         usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, BOOL *stop))block;

- (void)_enumerateKeysInAllCollectionsUsingBlock:
                            (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, BOOL *stop))block;

- (void)_enumerateKeysAndMetadataInCollection:(nullable NSString *)collection
                           usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id metadata, BOOL *stop))block;
- (void)_enumerateKeysAndMetadataInCollection:(nullable NSString *)collection
                           usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id metadata, BOOL *stop))block
                           withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *key))filter;

- (void)_enumerateKeysAndMetadataInCollections:(NSArray *)collections
                usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id metadata, BOOL *stop))block;
- (void)_enumerateKeysAndMetadataInCollections:(NSArray *)collections
                usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id metadata, BOOL *stop))block
                withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateKeysAndMetadataInAllCollectionsUsingBlock:
                (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id metadata, BOOL *stop))block;
- (void)_enumerateKeysAndMetadataInAllCollectionsUsingBlock:
                (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id metadata, BOOL *stop))block
     withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateKeysAndObjectsInCollection:(nullable NSString *)collection
        usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id object, BOOL *stop))block;
- (void)_enumerateKeysAndObjectsInCollection:(nullable NSString *)collection
        usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id object, BOOL *stop))block
        withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *key))filter;

- (void)_enumerateKeysAndObjectsInCollections:(NSArray<NSString*> *)collections
       usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, BOOL *stop))block;
- (void)_enumerateKeysAndObjectsInCollections:(NSArray<NSString*> *)collections
        usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, BOOL *stop))block
        withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateKeysAndObjectsInAllCollectionsUsingBlock:
                  (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, BOOL *stop))block;
- (void)_enumerateKeysAndObjectsInAllCollectionsUsingBlock:
                  (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, BOOL *stop))block
       withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateRowsInCollection:(nullable NSString *)collection
                usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id object, id metadata, BOOL *stop))block;
- (void)_enumerateRowsInCollection:(nullable NSString *)collection
                usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *key, id object, id metadata, BOOL *stop))block
                withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *key))filter;

- (void)_enumerateRowsInCollections:(NSArray<NSString*> *)collections
     usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, id metadata, BOOL *stop))block;
- (void)_enumerateRowsInCollections:(NSArray<NSString*> *)collections
     usingBlock:(void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, id metadata, BOOL *stop))block
     withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateRowsInAllCollectionsUsingBlock:
     (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, id metadata, BOOL *stop))block;
- (void)_enumerateRowsInAllCollectionsUsingBlock:
     (void (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key, id object, id metadata, BOOL *stop))block
  withFilter:(nullable BOOL (NS_NOESCAPE^)(int64_t rowid, NSString *collection, NSString *key))filter;

- (void)_enumerateRowidsForKeys:(NSArray<NSString*> *)keys
                   inCollection:(NSString *)collection
            unorderedUsingBlock:(void (NS_NOESCAPE^)(NSUInteger keyIndex, int64_t rowid, BOOL *stop))block;

@end

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma mark -
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

@interface YapDatabaseReadWriteTransaction () {
@public
	NSMutableArray<dispatch_queue_t> *completionQueueStack;
	NSMutableArray<dispatch_block_t> *completionBlockStack;
	
	BOOL rollback;
	id customObjectForNotification;
}

- (void)replaceObject:(id)object
               forKey:(NSString *)key
         inCollection:(nullable NSString *)collection
            withRowid:(int64_t)rowid
     serializedObject:(nullable NSData *)preSerializedObject;

- (void)replaceMetadata:(id)metadata
                 forKey:(NSString *)key
           inCollection:(nullable NSString *)collection
              withRowid:(int64_t)rowid
     serializedMetadata:(nullable NSData *)preSerializedMetadata;

- (void)removeObjectForCollectionKey:(YapCollectionKey *)collectionKey withRowid:(int64_t)rowid;
- (void)removeObjectForKey:(NSString *)key inCollection:(NSString *)collection withRowid:(int64_t)rowid;

- (void)addRegisteredExtensionTransaction:(YapDatabaseExtensionTransaction *)extTrnsactn withName:(NSString *)extName;
- (void)removeRegisteredExtensionTransactionWithName:(NSString *)extName;

- (void)setBoolValue:(BOOL)value         forKey:(NSString *)key extension:(NSString *)extensionName;
- (void)setIntValue:(int)value           forKey:(NSString *)key extension:(NSString *)extensionName;
- (void)setDoubleValue:(double)value     forKey:(NSString *)key extension:(NSString *)extensionName;
- (void)setStringValue:(NSString *)value forKey:(NSString *)key extension:(NSString *)extensionName;
- (void)setDataValue:(NSData *)value     forKey:(NSString *)key extension:(NSString *)extensionName;

- (void)removeValueForKey:(NSString *)key extension:(NSString *)extensionName;
- (void)removeAllValuesForExtension:(NSString *)extensionName;

@end

NS_ASSUME_NONNULL_END
