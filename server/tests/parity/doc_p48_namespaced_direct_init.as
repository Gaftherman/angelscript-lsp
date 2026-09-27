// Verified by angelscript_oracle: accepted
namespace meta_api {
    class Logger {
        Logger(const string &in name, bool isStatic = false) {}
    }
    namespace json {
        Logger g_Logger("JSON");
    }
}
