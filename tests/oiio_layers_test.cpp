/*
 * oiio_layers_test — ReadOIIO's mapping of EXR channel names to views and layers
 * (ReadOIIOPlugin::getLayers), on in-memory ImageSpecs: no file and no host needed.
 *
 * Multi-view EXRs: OpenEXR names channels layer.view.channel, with the default (first)
 * view's channels unprefixed; Nuke writes view.layer.channel (issue #75), e.g. for views
 * left (hero) and right: R,G,B,A, depth.Z, right.R,G,B,A, right.depth.Z.
 */
#include "../OIIO/ReadOIIO.cpp"

#include <cstdio>

namespace { // ReadOIIOPlugin is in ReadOIIO.cpp's anonymous namespace
struct ReadOIIOTestAccess {
    static void getLayers(const vector<ImageSpec>& subimages, ViewsLayersMap* layersMap)
    {
        ReadOIIOPlugin::getLayers(subimages, layersMap, nullptr);
    }
};
} // anon namespace

static int failures = 0;

static ImageSpec
makeSpec(const vector<string>& channels, const vector<string>& views)
{
    ImageSpec spec(8, 8, (int)channels.size(), TypeDesc::HALF);
    spec.channelnames = channels;
    if (!views.empty()) {
        vector<ustring> v(views.begin(), views.end());
        spec.attribute("multiView", TypeDesc(TypeDesc::STRING, (int)v.size()), v.data());
    }
    return spec;
}

// expected: "view/layer:channelIndexes" sorted per view, layer order as in the file
static string
describe(const ViewsLayersMap& m)
{
    string s;
    for (const auto& v : m) {
        for (const auto& l : v.second) {
            s += v.first + "/" + l.first + ":";
            for (size_t k = 0; k < l.second.channelIndexes.size(); ++k) {
                s += (k ? "," : "") + std::to_string(l.second.channelIndexes[k]);
            }
            s += " ";
        }
    }
    return s;
}

static void
check(const char* name, const vector<string>& channels, const vector<string>& views, const string& expected)
{
    ViewsLayersMap m;
    ReadOIIOTestAccess::getLayers(vector<ImageSpec>(1, makeSpec(channels, views)), &m);
    const string got = describe(m);
    if (got == expected) {
        std::printf("  ok   %s\n", name);
    } else {
        std::printf("  FAIL %s\n    expected: %s\n    got:      %s\n", name, expected.c_str(), got.c_str());
        ++failures;
    }
}

int
main()
{
    std::printf("==== ReadOIIO EXR layer/view mapping ====\n");
    // openexr-images MultiView/Balls.exr layout
    check("OpenEXR default view unprefixed, view.channel",
          { "R", "G", "B", "right.R", "right.G", "right.B" }, { "left", "right" },
          "left/Color:0,1,2 right/Color:3,4,5 ");
    // OpenEXR spec: layer.view.channel
    check("OpenEXR layer.view.channel",
          { "R", "G", "B", "A", "depth.Z", "right.R", "right.G", "right.B", "right.A", "depth.right.Z" }, { "left", "right" },
          "left/Color:0,1,2,3 left/depth:4 right/Color:5,6,7,8 right/depth:9 ");
    // Nuke: view.layer.channel (#75)
    check("Nuke view.layer.channel",
          { "R", "G", "B", "A", "depth.Z", "right.R", "right.G", "right.B", "right.A", "right.depth.Z" }, { "left", "right" },
          "left/Color:0,1,2,3 left/depth:4 right/Color:5,6,7,8 right/depth:9 ");
    // Nuke, both views prefixed, nested layer name
    check("Nuke view.layer.sub.channel",
          { "left.R", "left.G", "left.B", "left.diffuse.direct.R", "left.diffuse.direct.G", "left.diffuse.direct.B",
            "right.R", "right.G", "right.B", "right.diffuse.direct.R", "right.diffuse.direct.G", "right.diffuse.direct.B" },
          { "left", "right" },
          "left/Color:0,1,2 left/diffuse.direct:3,4,5 right/Color:6,7,8 right/diffuse.direct:9,10,11 ");
    // view names are matched case-insensitively, and reported as declared in multiView
    check("OpenEXR layer.view.channel, view name case differs",
          { "R", "G", "B", "depth.Z", "Right.R", "Right.G", "Right.B", "depth.Right.Z" }, { "left", "right" },
          "left/Color:0,1,2 left/depth:3 right/Color:4,5,6 right/depth:7 ");
    check("Nuke view.layer.channel, view name case differs",
          { "R", "G", "B", "depth.Z", "RIGHT.R", "RIGHT.G", "RIGHT.B", "RIGHT.depth.Z" }, { "left", "right" },
          "left/Color:0,1,2 left/depth:3 right/Color:4,5,6 right/depth:7 ");
    // ambiguous: both the first and the penultimate components are views: OpenEXR's layer.view.channel wins
    check("ambiguous view.view.channel: OpenEXR reading",
          { "R", "G", "B", "right.left.Z" }, { "left", "right" },
          "left/Color:0,1,2 left/right:3 ");
    // no multiView attribute: everything before the last dot is the layer name
    check("single view, nested layer names",
          { "R", "G", "B", "right.depth.Z" }, {},
          "Main/Color:0,1,2 Main/right.depth:3 ");
    std::printf(failures ? "SOME LAYER TESTS FAILED\n" : "ALL LAYER TESTS PASSED\n");
    return failures ? 1 : 0;
}
