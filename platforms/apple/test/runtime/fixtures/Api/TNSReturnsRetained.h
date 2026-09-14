id functionReturnsNSRetained() NS_RETURNS_RETAINED;
id functionReturnsCFRetained() CF_RETURNS_RETAINED NS_RETURNS_RETAINED;

CF_IMPLICIT_BRIDGING_ENABLED
CFTypeRef functionImplicitCreate();
CF_IMPLICIT_BRIDGING_DISABLED

id functionExplicitCreateNSObject() NS_RETURNS_RETAINED;

@interface TNSReturnsRetained : NSObject
+ (id)methodReturnsNSRetained NS_RETURNS_RETAINED;
+ (id)methodReturnsCFRetained CF_RETURNS_RETAINED NS_RETURNS_RETAINED;
+ (id)newNSObjectMethod;
@end
