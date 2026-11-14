// @expect: reject
// @complete- SecretCode
// @complete- SecretKey
class VaultBox {
    private int SecretCode;
    private int SecretKey;
    int PublicId;
}
void breach(VaultBox& v) {
    int stolen = v.Se/*$complete*/;
}
