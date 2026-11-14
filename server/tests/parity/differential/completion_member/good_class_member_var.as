// @expect: accept
// @complete+ FlintSpark
// @complete+ FlintEdge
// @complete- FlintMine
class FlintMine {}
class FlintTool {
    int FlintSpark;
    int FlintEdge;
}
void strike(FlintTool& tool) {
    int x = tool.Fl/*$complete*/;
}
