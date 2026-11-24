class WeaponCache{
int ammoCount;
WeaponCache(int count){
ammoCount=count;
}
void Restock(int added){
ammoCount+=added;
}
}