/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2025, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

* Redistributions of source code must retain the above
copyright notice, this list of conditions and the
following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the
following disclaimer in the documentation and/or other
materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
contributors may be used to endorse or promote products
derived from this software without specific prior
written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/
#include "UnitTestPCH.h"

#include <assimp/Exporter.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/scene.h>

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_EXPORT

class TestProgressHandler : public ProgressHandler {
public:
    TestProgressHandler() :
            ProgressHandler(),
            mPercentage(0.f) {
        // empty
    }

    virtual ~TestProgressHandler() = default;

    bool Update(float percentage = -1.f) override {
        mPercentage = percentage;
        return true;
    }
    float mPercentage;
};

class ExporterTest : public ::testing::Test {
    // empty
};

TEST_F(ExporterTest, ProgressHandlerTest) {
    Exporter exporter;
    TestProgressHandler *ph(new TestProgressHandler);
    exporter.SetProgressHandler(ph);
}

namespace {

int s_exportCalls = 0;

void CountedExport(const char *, IOSystem *, const aiScene *, const ExportProperties *) {
    ++s_exportCalls;
}

class CancellingExportProgressHandler : public ProgressHandler {
public:
    explicit CancellingExportProgressHandler(size_t cancelOnUpdate) :
            cancelOnUpdate(cancelOnUpdate) {}

    bool Update(float percentage = -1.f) override {
        updates.push_back(percentage);
        return !cancel;
    }

    void UpdateFileWrite(int currentStep, int) override {
        fileWriteSteps.push_back(currentStep);
        cancel = cancelOnUpdate != 0 && fileWriteSteps.size() == cancelOnUpdate;
    }

    size_t cancelOnUpdate;
    bool cancel = false;
    std::vector<float> updates;
    std::vector<int> fileWriteSteps;
};

class ExportProgressCancellationTest : public ::testing::Test {
protected:
    void ExpectCancellationAndRecovery(size_t cancelOnUpdate, int exportsBeforeRecovery) {
        aiScene scene;
        scene.mRootNode = new aiNode;
        Exporter exporter;
        ASSERT_EQ(AI_SUCCESS, exporter.RegisterExporter(
                                      Exporter::ExportFormatEntry(
                                              "progress", "Progress test", "progress", CountedExport)));
        CancellingExportProgressHandler *progress = new CancellingExportProgressHandler(cancelOnUpdate);
        exporter.SetProgressHandler(progress);
        s_exportCalls = 0;

        EXPECT_EQ(AI_FAILURE, exporter.Export(&scene, "progress", "ignored"));
        EXPECT_STREQ("Export cancelled by progress handler", exporter.GetErrorString());
        EXPECT_EQ(exportsBeforeRecovery, s_exportCalls);
        EXPECT_EQ(cancelOnUpdate, progress->fileWriteSteps.size());
        EXPECT_EQ(progress->fileWriteSteps.size(), progress->updates.size());

        progress->cancelOnUpdate = 0;
        progress->cancel = false;
        progress->updates.clear();
        progress->fileWriteSteps.clear();
        EXPECT_EQ(AI_SUCCESS, exporter.Export(&scene, "progress", "ignored"));
        EXPECT_STREQ("", exporter.GetErrorString());
        EXPECT_EQ(exportsBeforeRecovery + 1, s_exportCalls);
        ASSERT_EQ(5u, progress->fileWriteSteps.size());
        EXPECT_EQ(progress->fileWriteSteps.size(), progress->updates.size());
        for (size_t i = 0; i < progress->fileWriteSteps.size(); ++i) {
            EXPECT_EQ(static_cast<int>(i), progress->fileWriteSteps[i]);
        }
    }
};

} // namespace

TEST_F(ExportProgressCancellationTest, defaultProgressHandlerContinues) {
    aiScene scene;
    scene.mRootNode = new aiNode;
    Exporter exporter;
    ASSERT_EQ(AI_SUCCESS, exporter.RegisterExporter(
                                  Exporter::ExportFormatEntry(
                                          "progress", "Progress test", "progress", CountedExport)));
    s_exportCalls = 0;

    EXPECT_EQ(AI_SUCCESS, exporter.Export(&scene, "progress", "ignored"));
    EXPECT_STREQ("", exporter.GetErrorString());
    EXPECT_EQ(1, s_exportCalls);
}

TEST_F(ExportProgressCancellationTest, beforeSceneCopy) {
    ExpectCancellationAndRecovery(1, 0);
}

TEST_F(ExportProgressCancellationTest, afterSceneCopy) {
    ExpectCancellationAndRecovery(2, 0);
}

TEST_F(ExportProgressCancellationTest, beforePostProcessing) {
    ExpectCancellationAndRecovery(3, 0);
}

TEST_F(ExportProgressCancellationTest, beforeExport) {
    ExpectCancellationAndRecovery(4, 0);
}

TEST_F(ExportProgressCancellationTest, afterExport) {
    ExpectCancellationAndRecovery(5, 1);
}

// Make sure all the registered exporters have useful descriptions
TEST_F(ExporterTest, ExporterIdTest) {
    Exporter exporter;
    size_t exportFormatCount = exporter.GetExportFormatCount();
    EXPECT_NE(0u, exportFormatCount) << "No registered exporters";
    typedef std::map<std::string, const aiExportFormatDesc *> ExportIdMap;
    ExportIdMap exporterMap;
    for (size_t i = 0; i < exportFormatCount; ++i) {
        // Check that the exporter description exists and makes sense
        const aiExportFormatDesc *desc = exporter.GetExportFormatDescription(i);
        ASSERT_NE(nullptr, desc) << "Missing aiExportFormatDesc at index " << i;
        EXPECT_NE(nullptr, desc->id) << "Null exporter ID at index " << i;
        EXPECT_STRNE("", desc->id) << "Empty exporter ID at index " << i;
        EXPECT_NE(nullptr, desc->description) << "Null exporter description at index " << i;
        EXPECT_STRNE("", desc->description) << "Empty exporter description at index " << i;
        EXPECT_NE(nullptr, desc->fileExtension) << "Null exporter file extension at index " << i;
        EXPECT_STRNE("", desc->fileExtension) << "Empty exporter file extension at index " << i;

        // Check the ID is unique
        std::string key(desc->id);
        std::pair<ExportIdMap::iterator, bool> result = exporterMap.emplace(key, desc);
        EXPECT_TRUE(result.second) << "Duplicate exported id: '" << key << "' " << desc->description << " *." << desc->fileExtension << " at index " << i;
    }

    const aiExportFormatDesc *desc = exporter.GetExportFormatDescription(exportFormatCount);
    EXPECT_EQ(nullptr, desc) << "More exporters than claimed";
}

#endif
