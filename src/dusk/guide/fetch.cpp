#include "dusk/guide/fetch.hpp"

#include <atomic>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "dusk/guide/store.hpp"
#include "dusk/http/http.hpp"
#include "version.h"

namespace dusk::guide {

// Internal: the import worker's image supplier. Was in the header, but nothing
// outside this file ever used it.
ImageSource network_image_source();

namespace {


std::string user_agent() {
    return fmt::format("Dusklight/{} (+guide-import)", DUSK_WC_DESCRIBE);
}





// Same shape as FetchTask: worker thread, atomic done flag, joined on take.
class ImportTask {
public:
    ImportTask() {
        mWorker = std::thread([this] {
            int n = 0;
            try {
                n = scan_import_folder(network_image_source());
            } catch (...) {
                n = 0;
            }
            mCount = n;
            mDone.store(true, std::memory_order_release);
        });
    }
    ~ImportTask() {
        if (mWorker.joinable()) {
            mWorker.join();
        }
    }
    ImportTask(const ImportTask&) = delete;
    ImportTask& operator=(const ImportTask&) = delete;
    bool finished() const { return mDone.load(std::memory_order_acquire); }
    int count() const { return mCount; }

private:
    std::thread mWorker;
    std::atomic<bool> mDone{false};
    int mCount = 0;
};

std::unique_ptr<ImportTask> s_import;
unsigned s_importGen = 0;
int s_lastImportCount = 0;

// Joins and clears a finished worker, exactly once, whichever consumer asks
// first. Game thread only.
void reap_import() {
    if (s_import != nullptr && s_import->finished()) {
        s_lastImportCount = s_import->count();
        s_import.reset();  // joins
        s_importGen++;
    }
}

}  // namespace

void begin_import() {
    if (s_import == nullptr) {
        s_import = std::make_unique<ImportTask>();
    }
}

bool import_in_progress() {
    reap_import();
    return s_import != nullptr;
}

unsigned import_generation() {
    reap_import();
    return s_importGen;
}

int last_import_count() {
    return s_lastImportCount;
}

ImageSource network_image_source() {
    if (!http::available()) {
        return {};
    }
    return [](const std::string& url, std::string& out) {
        if (url.compare(0, 8, "https://") != 0) {
            return false;
        }
        http::Request ir{
            .url = url,
            .headers = {{.name = "User-Agent", .value = user_agent()}},
            .timeout = std::chrono::milliseconds(10000),
            .maxBodyBytes = 4u * 1024u * 1024u,
        };
        const http::Result r = http::get(ir);
        if (r.error != http::Error::None || r.response.statusCode != 200) {
            return false;
        }
        out = r.response.body;
        return true;
    };
}

}  // namespace dusk::guide
