#pragma once

#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <functional>
#include <chrono>
#include <vector>
#include <deque>
#include <iterator>
#include <iostream>
#include <mutex>
#include <map>
#include <atomic>

#include <assert.h>
#include <sys/select.h>
#include <sys/time.h>

#include <curl/curl.h>
#include <curl/easy.h>


// Max concurrent connections
#ifndef RESTINCURL_MAX_CONNECTIONS
#   define RESTINCURL_MAX_CONNECTIONS 32L
#endif

#ifndef RESTINCURL_ENABLE_ASYNC
#   define RESTINCURL_ENABLE_ASYNC 1
#endif


namespace restincurl {

    using lock_t = std::lock_guard<std::mutex>;

    enum class RequestType { GET, PUT, POST, HEAD, DELETE, PATCH, OPTIONS, INVALID };
    using completion_fn_t = std::function<void (CURLcode result)>;

    class Exception : public std::runtime_error {
    public:
        Exception(const std::string& msg) : runtime_error(msg) {}
    };

    class CurlException : public Exception {
    public:
        CurlException(const std::string msg, const CURLcode err)
            : Exception(msg + '(' + std::to_string(err) + "): " + curl_easy_strerror(err))
            , err_{err}
            {}

         CurlException(const std::string msg, const CURLMcode err)
            : Exception(msg + '(' + std::to_string(err) + "): " + curl_multi_strerror(err))
            , err_{err}
            {}

        int getErrorCode() const noexcept { err_; }

    private:
        const int err_;
    };

    class EasyHandle {
    public:
        using ptr_t = std::unique_ptr<EasyHandle>;
        using handle_t = decltype(curl_easy_init());

        EasyHandle()
        {
            std::clog << "EasyHandle created: " << handle_ << std::endl;
        }

        ~EasyHandle() {
            Close();
        }

        void Close() {
            if (handle_) {
                std::clog << "Cleaning easy-handle " << handle_ << std::endl;
                curl_easy_cleanup(handle_);
                handle_ = nullptr;
            }
        }

        operator handle_t () const noexcept { return handle_; }

    private:
        handle_t handle_ = curl_easy_init();
    };

    class Options {
    public:
        Options(EasyHandle& eh) : eh_{eh} {}

        template <typename T>
        Options& Set(const CURLoption& opt, const T& value) {
            const auto ret = curl_easy_setopt(eh_, opt, value);
            if (ret) {
                throw CurlException(
                    std::string("Setting option ") + std::to_string(opt), ret);
            }
            return *this;
        }

        Options& Set(const CURLoption& opt, const std::string& value) {
            return Set(opt, value.c_str());
        }

    private:
        EasyHandle& eh_;
    };

    struct DataHandlerBase {
        virtual ~DataHandlerBase() = default;
    };

    template <typename T>
    struct InDataHandler : public DataHandlerBase{
        InDataHandler(T& data) : data_{data} {
            std::clog << "InDataHandler address: " << this << std::endl;
        }

        static size_t write_callback(char *ptr, size_t size, size_t nitems, void *userdata) {
            assert(userdata);
            InDataHandler *self = reinterpret_cast<InDataHandler *>(userdata);
            const auto bytes = size * nitems;
            if (bytes > 0) {
                std::copy(ptr, ptr + bytes, std::back_inserter(self->data_));
            }
            return bytes;
        }

        T& data_;
    };

    template <typename T>
    struct OutDataHandler : public DataHandlerBase {
        OutDataHandler() = default;
        OutDataHandler(const T& v) : data_{v} {}
        OutDataHandler(T&& v) : data_{std::move(v)} {}

        static size_t read_callback(char *bufptr, size_t size, size_t nitems, void *userdata) {
            assert(userdata);
            OutDataHandler *self = reinterpret_cast<OutDataHandler *>(userdata);
            const auto bytes = size * nitems;
            auto out_bytes= std::min<size_t>(bytes, (self->data_.size() - self->sendt_bytes_));
            std::copy(self->data_.cbegin() + self->sendt_bytes_,
                      self->data_.cbegin() + out_bytes,
                      bufptr);
            self->sendt_bytes_ += out_bytes;
            return out_bytes;
        }

        T data_;
        size_t sendt_bytes_ = 0;
    };

    class Request {
    public:
        using ptr_t = std::unique_ptr<Request>;

        Request()
        : eh_{std::make_unique<EasyHandle>()}
        {
        }

        Request(EasyHandle::ptr_t&& eh)
        : eh_{std::move(eh)}
        {
        }

        ~Request() {
            if (headers_) {
                curl_slist_free_all(headers_);
            }
        }

        void Prepare(const RequestType rq, completion_fn_t completion) {
            request_type_ = rq;;
            SetRequestType();
            completion_ = std::move(completion);
        }

        // Synchronous execution.
        void Execute() {
            const auto result = curl_easy_perform(*eh_);
            if (completion_) {
                completion_(result);
            }
        }

        void Complete(CURLcode cc, const CURLMSG& /*msg*/) {
            if (completion_) {
                completion_(cc);
            }
        }

        EasyHandle& GetEasyHandle() noexcept { assert(eh_); return *eh_; }
        RequestType GetRequestType() noexcept { return request_type_; }

        void SetDefaultInHandler(std::unique_ptr<DataHandlerBase> ptr) {
            default_in_handler_ = std::move(ptr);
        }

        void SetDefaultOutHandler(std::unique_ptr<DataHandlerBase> ptr) {
            default_out_handler_ = std::move(ptr);
        }

        using headers_t = curl_slist *;
        headers_t& GetHeaders() {
            return headers_;
        }

    private:
        void SetRequestType() {
            switch(request_type_) {
                case RequestType::GET:
                    curl_easy_setopt(*eh_, CURLOPT_HTTPGET, 1L);
                    break;
                case RequestType::PUT:
                    curl_easy_setopt(*eh_, CURLOPT_PUT, 1L);
                    break;
                case RequestType::POST:
                    curl_easy_setopt(*eh_, CURLOPT_POST, 1L);
                    break;
                case RequestType::HEAD:
                    curl_easy_setopt(*eh_, CURLOPT_NOBODY, 1L);
                    break;
                case RequestType::OPTIONS:
                    curl_easy_setopt(*eh_, CURLOPT_CUSTOMREQUEST, "OPTIONS");
                    break;
                case RequestType::PATCH:
                    curl_easy_setopt(*eh_, CURLOPT_CUSTOMREQUEST, "PATCH");
                    break;
                case RequestType::DELETE:
                    curl_easy_setopt(*eh_, CURLOPT_CUSTOMREQUEST, "DELETE");
                    break;
                default:
                    throw Exception("Unsupported request type" + std::to_string(static_cast<int>(request_type_)));
            }
        }

        EasyHandle::ptr_t eh_;
        RequestType request_type_ = RequestType::INVALID;
        completion_fn_t completion_;
        std::unique_ptr<DataHandlerBase> default_out_handler_;
        std::unique_ptr<DataHandlerBase> default_in_handler_;
        headers_t headers_ = nullptr;
    };

#if RESTINCURL_ENABLE_ASYNC
    class Worker {
    public:
        Worker()
        : thread_{[&] {
            try {
                std::clog << "Starting thread " << std::this_thread::get_id() << std::endl;
                Init();
                Run();
                Clean();
            } catch (const std::exception& ex) {
                std::clog << "Worker: " << ex.what();
            }
            std::clog << "Exiting thread " << std::this_thread::get_id() << std::endl;
        }} {}

        ~Worker() {
            if (thread_.joinable()) {
                thread_.detach();
            }
        }

        static std::unique_ptr<Worker> Create() {
            return std::make_unique<Worker>();
        }

        void Enqueue(Request::ptr_t req) {
            std::clog << "Queuing request " << std::endl;
            lock_t lock(mutex_);
            queue_.push_back(std::move(req));
            Signal();
        }

        void Join() const {
            std::call_once(joined_, [this] {
                const_cast<Worker *>(this)->thread_.join();
            });
        }

        // Let the current transfers complete, then quit
        void CloseWhenFinished() {
            close_pending_ = true;
        }

        // Shut down now. Abort all transfers
        void Close() {
            abort_ = true;
            Signal();
        }

        // Check if the run loop has finished.
        bool IsDone() const {
            return done_;
        }

    private:
        void Signal() {
            //TODO: Implement
        }

        void Dequeue() {
            lock_t lock(mutex_);

            for(auto& req: queue_) {
                assert(req);
                const auto& eh = req->GetEasyHandle();
                std::clog << "Adding request: " << eh << std::endl;
                ongoing_[eh] = std::move(req);
                const auto mc = curl_multi_add_handle(handle_, eh);
                if (mc != CURLM_OK) {
                    throw CurlException("curl_multi_add_handle", mc);
                }
            }

            queue_.clear();
        }

        void Init() {
            if ((handle_ = curl_multi_init()) == nullptr) {
                throw std::runtime_error("curl_multi_init() failed");
            }

            curl_multi_setopt(handle_, CURLMOPT_MAXCONNECTS, RESTINCURL_MAX_CONNECTIONS);
        }

        void Clean() {
            if (handle_) {
                std::clog << "Calling curl_multi_cleanup: " << handle_ << std::endl;
                curl_multi_cleanup(handle_);
                handle_ = nullptr;
            }
        }

        void Run() {
            int transfers_running = -1;
            fd_set fdread = {};
            fd_set fdwrite = {};
            fd_set fdexcep = {};

            while (!abort_ && (transfers_running || !close_pending_)) {
                Dequeue();

                 /* timeout or readable/writable sockets */
                curl_multi_perform(handle_, &transfers_running);

                int numLeft = {};
                while (auto m = curl_multi_info_read(handle_, &numLeft)) {
                    assert(m);
                    auto it = ongoing_.find(m->easy_handle);
                    if (it != ongoing_.end()) {
                        std::clog << "Finishing request with easy-handle: "
                            << (EasyHandle::handle_t)it->second->GetEasyHandle() << std::endl;
                        try {
                            it->second->Complete(m->data.result, m->msg);
                        } catch(const std::exception& ex) {
                            std::clog << "Complete threw: " << ex.what() << std::endl;
                        }
                        if (m->msg == CURLMSG_DONE) {
                            curl_multi_remove_handle(handle_, m->easy_handle);
                        }
                        it->second->GetEasyHandle().Close();
                        ongoing_.erase(it);
                    } else {
                        std::clog << "Failed to find easy_handle in ongoing!" << std::endl;
                        assert(false);
                    }
                }

                int maxfd = -1;

                long timeout;
                /* extract timeout value */

                curl_multi_timeout(handle_, &timeout);
                if (timeout < 0) {
                    timeout = 1000;
                }

                struct timeval tv = {};
                tv.tv_sec = timeout / 1000;
                tv.tv_usec = (timeout % 1000) * 1000;

                FD_ZERO(&fdread);
                FD_ZERO(&fdwrite);
                FD_ZERO(&fdexcep);

                /* get file descriptors from the transfers */
                const auto mc = curl_multi_fdset(handle_, &fdread, &fdwrite, &fdexcep, &maxfd);
                if (mc != CURLM_OK) {
                    throw CurlException("curl_multi_fdset", mc);
                }
                if (maxfd == -1) {
                    // TODO: Remove when we have pipe
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    select(maxfd+1, &fdread, &fdwrite, &fdexcep, &tv);
                }
            }

            done_ = true;
        }

        std::atomic_bool close_pending_ {false};
        std::atomic_bool abort_ {false};
        std::atomic_bool done_ {false};
        decltype(curl_multi_init()) handle_ = {};
        std::mutex mutex_;
        std::thread thread_;
        std::deque<Request::ptr_t> queue_;
        std::map<EasyHandle::handle_t, Request::ptr_t> ongoing_;
        mutable std::once_flag joined_;
    };
#endif // RESTINCURL_ENABLE_ASYNC

    // Convenience interface
    // We can use different containers for default data handling, but for
    // json, strings are usually OK. We can still call SendData<>() and StoreData<>()
    // with different template parameters, or even set our own Curl compatible
    // read / write handlers.
    class RequestBuilder {
        // noop handler for incoming data
        static size_t write_callback(char *ptr, size_t size, size_t nitems, void *userdata) {
            const auto bytes = size * nitems;
            return bytes;
        }
    public:
        using ptr_t = std::unique_ptr<RequestBuilder>;
        RequestBuilder(
#if RESTINCURL_ENABLE_ASYNC
            Worker& worker
#endif
        )
        : request_{std::make_unique<Request>()}
        , options_{std::make_unique<Options>(request_->GetEasyHandle())}
#if RESTINCURL_ENABLE_ASYNC
        , worker_(worker)
#endif
        {}

        ~RequestBuilder() {
        }

    protected:
        RequestBuilder& Prepare(RequestType rt, const std::string& url) {
            assert(request_type_ == RequestType::INVALID);
            assert(!is_built_);
            request_type_  = rt;
            url_ = url;
            return *this;
        }

    public:
        RequestBuilder& Get(const std::string& url) {
            return Prepare(RequestType::GET, url);
        }

        RequestBuilder& Head(const std::string& url) {
            return Prepare(RequestType::HEAD, url);
        }

        RequestBuilder& Post(const std::string& url) {
            return Prepare(RequestType::POST, url);
        }

        RequestBuilder& Put(const std::string& url) {
            return Prepare(RequestType::PUT, url);
        }

        RequestBuilder& Delete(const std::string& url) {
            return Prepare(RequestType::DELETE, url);
        }

        RequestBuilder& Header(const char *value) {
            assert(value);
            assert(!is_built_);
            request_->GetHeaders() = curl_slist_append(request_->GetHeaders(), value);
            return *this;
        }

        RequestBuilder& Header(const std::string& name,
                               const std::string& value) {
            const auto v = name + ": " + value;
            return Header(v.c_str());
        }

        RequestBuilder& WithJson() {
            return Header("Content-type: Application/json; charset=utf-8");
        }

        RequestBuilder& AcceptJson() {
            return Header("Accept: Application/json");
        }

        template <typename T>
        RequestBuilder& Option(const CURLoption& opt, const T& value) {
            assert(!is_built_);
            options_->Set(opt, value);
            return *this;
        }

        // Outgoing data
        template <typename T>
        RequestBuilder& SendData(OutDataHandler<T>& dh) {
            assert(!is_built_);
            options_->Set(CURLOPT_READFUNCTION, dh.read_callback);
            options_->Set(CURLOPT_READDATA, &dh);
            have_data_out_ = true;
            return *this;
        }

        template <typename T>
        RequestBuilder& SendData(T data) {
            assert(!is_built_);
            auto handler = std::make_unique<OutDataHandler<T>>(std::move(data));
            auto& handler_ref = *handler;
            request_->SetDefaultOutHandler(std::move(handler));
            return SendData(handler_ref);
        }

        // Incoming data
        template <typename T>
        RequestBuilder& StoreData(InDataHandler<T>& dh) {
            assert(!is_built_);
            options_->Set(CURLOPT_WRITEFUNCTION, dh.write_callback);
            options_->Set(CURLOPT_WRITEDATA, &dh);
            have_data_in_ = true;
            return *this;
        }

        // Store data in the provided string. It must exist until the transfer is complete.
        template <typename T>
        RequestBuilder& StoreData(T& data) {
            assert(!is_built_);
            auto handler = std::make_unique<InDataHandler<T>>(data);
            auto& handler_ref = *handler;
            request_->SetDefaultInHandler(std::move(handler));
            return StoreData(handler_ref);
        }

        RequestBuilder& WithCompletion(completion_fn_t fn) {
            assert(!is_built_);
            completion_ = std::move(fn);
            return *this;
        }

        // Set Curl compatible read handler. You will probably don't need this.
        RequestBuilder& SetReadHandler(size_t (*handler)(char *, size_t , size_t , void *), void *userdata) {
            options_->Set(CURLOPT_READFUNCTION, handler);
            options_->Set(CURLOPT_READDATA, userdata);
            have_data_out_ = true;
            return *this;
        }

        // Set Curl compatible write handler. You will probably don't need this.
        RequestBuilder& SetWriteHandler(size_t (*handler)(char *, size_t , size_t , void *), void *userdata) {
            options_->Set(CURLOPT_WRITEFUNCTION,handler);
            options_->Set(CURLOPT_WRITEDATA, userdata);
            have_data_in_ = true;
            return *this;
        }

        void Build() {
            if (!is_built_) {
                // Set up data handlers
                if (!have_data_in_) {
                    options_->Set(CURLOPT_WRITEFUNCTION, write_callback);
                }

                if (have_data_out_) {
                    options_->Set(CURLOPT_UPLOAD, 1L);
                }

                // Set headers
                if (request_->GetHeaders()) {
                    options_->Set(CURLOPT_HTTPHEADER, request_->GetHeaders());
                }

                // TODO: Set up timeout
                // TODO: Prepare the final url (we want nice, correctly encoded request arguments)
                options_->Set(CURLOPT_URL, url_);

                // Prepare request
                request_->Prepare(request_type_, std::move(completion_));
                is_built_ = true;
            }
        }

        void ExecuteSynchronous() {
            Build();
            request_->Execute();
        }

#if RESTINCURL_ENABLE_ASYNC
        void Execute() {
            Build();
            worker_.Enqueue(std::move(request_));
        }
#endif

    private:
        std::unique_ptr<Request> request_;
        std::unique_ptr<Options> options_;
        std::string url_;
        RequestType request_type_ = RequestType::INVALID;
        bool have_data_in_ = false;
        bool have_data_out_ = false;
        bool is_built_ = false;
        completion_fn_t completion_;
#if RESTINCURL_ENABLE_ASYNC
        Worker& worker_;
#endif
    };


    class Client {

    public:
        // Set init to false if curl is already initialized by unrelated code
        Client(const bool init = true) {
            if (init) {
                static std::once_flag flag;
                std::call_once(flag, [] {
                    std::clog << "One time initialization of curl." << std::endl;
                    curl_global_init(CURL_GLOBAL_DEFAULT);
                });
            }
        }

        virtual ~Client() {
#if RESTINCURL_ENABLE_ASYNC
            if (worker_) {
                try {
                    worker_->Close();
                } catch (const std::exception& ex) {
                    std::clog << "~Client(): " << ex.what() << std::endl;
                }
            }
#endif
        }

        std::unique_ptr<RequestBuilder> Build() {
            return std::make_unique<RequestBuilder>(
#if RESTINCURL_ENABLE_ASYNC
                *worker_
#endif
            );
        }

#if RESTINCURL_ENABLE_ASYNC
        void CloseWhenFinished() {
            worker_->CloseWhenFinished();
        }

        void Close() {
            worker_->Close();
        }

        void WaitForFinish() {
            worker_->Join();
        }
#endif

    private:
#if RESTINCURL_ENABLE_ASYNC
        std::unique_ptr<Worker> worker_ = std::make_unique<Worker>();
#endif
    };


} // namespace
