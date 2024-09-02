#include "standalone.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <thread>

#include "macro.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define pipe _pipe
#define popen _popen
#define pclose _pclose
#else
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct LocalDriver {
    int processHandle{};
    int stdinFd{};
    int stdoutFd{};
    std::string name;
    bool isListening{};
};

#if defined(_WIN32) || defined(_WIN64)
constexpr char SEM_NAME[] = "driver_semaphore";
constexpr char SHM_NAME[] = "driver_shm";
#else
constexpr char SEM_NAME[] = "/driver_semaphore";
constexpr char SHM_NAME[] = "/driver_shm";
#endif

class StandAloneComponentImpl {
public:
    LocalDriver driver;
    std::atomic<bool> shouldExit{false};
    std::jthread driverThread;

    void handleDriverOutput(std::string_view driver_name,
                            std::span<const char> buffer) {
        LOG_F(INFO, "Output from driver {}: {}", driver_name,
              std::string_view(buffer.data(), buffer.size()));
    }

    void closePipes(int stdinPipe[2], int stdoutPipe[2]) {
#if defined(_WIN32) || defined(_WIN64)
        _close(stdinPipe[0]);
        _close(stdinPipe[1]);
        _close(stdoutPipe[0]);
        _close(stdoutPipe[1]);
#else
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
#endif
    }
};

StandAloneComponent::StandAloneComponent(std::string name)
    : Component(std::move(name)),
      impl_(std::make_unique<StandAloneComponentImpl>()) {
    doc("A standalone component that can be used to run a local driver");
    def("start", &StandAloneComponent::startLocalDriver);
    def("stop", &StandAloneComponent::stopLocalDriver);
    def("listen", &StandAloneComponent::toggleDriverListening);
    def("send", &StandAloneComponent::sendMessageToDriver);
    def("print", &StandAloneComponent::printDriver);
    def("monitor", &StandAloneComponent::monitorDrivers);
}

StandAloneComponent::~StandAloneComponent() {
    LOG_F(INFO, "Component {} destroyed", getName());
    impl_->shouldExit = true;
    if (impl_->driverThread.joinable()) {
        impl_->driverThread.join();
    }
}

void StandAloneComponent::startLocalDriver(const std::string& driver_name) {
    int stdinPipe[2];
    int stdoutPipe[2];

    if (!createPipes(stdinPipe, stdoutPipe)) {
        return;
    }

#if defined(_WIN32) || defined(_WIN64)
    startWindowsProcess(driver_name, stdinPipe, stdoutPipe);
#else
    startUnixProcess(driver_name, stdinPipe, stdoutPipe);
#endif
    impl_->driverThread =
        std::jthread(&StandAloneComponent::backgroundProcessing, this);
}

void StandAloneComponent::backgroundProcessing() {
    while (!impl_->shouldExit) {
        monitorDrivers();
        processMessages();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

auto StandAloneComponent::createPipes(int stdinPipe[2],
                                      int stdoutPipe[2]) -> bool {
#if defined(_WIN32) || defined(_WIN64)
    return _pipe(stdinPipe, 4096, _O_BINARY | _O_NOINHERIT) == 0 &&
           _pipe(stdoutPipe, 4096, _O_BINARY | _O_NOINHERIT) == 0;
#else
    return pipe(stdinPipe) == 0 && pipe(stdoutPipe) == 0;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
void StandAloneComponent::startWindowsProcess(const std::string& driver_name,
                                              int stdinPipe[2],
                                              int stdoutPipe[2]) {
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hStdinRead, hStdinWrite, hStdoutRead, hStdoutWrite;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0) ||
        !CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        LOG_F(ERROR, "Failed to create pipes");
        return;
    }

    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFO si = {sizeof(STARTUPINFO)};
    si.hStdError = hStdoutWrite;
    si.hStdOutput = hStdoutWrite;
    si.hStdInput = hStdinRead;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    std::string cmd = driver_name;

    if (!CreateProcess(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        LOG_F(ERROR, "Failed to start process");
        impl_->closePipes(stdinPipe, stdoutPipe);
        return;
    }

    CloseHandle(hStdoutWrite);
    CloseHandle(hStdinRead);

    impl_->driver.processHandle =
        static_cast<int>(reinterpret_cast<intptr_t>(pi.hProcess));

    impl_->driver.stdinFd =
        _open_osfhandle(reinterpret_cast<intptr_t>(hStdinWrite), 0);
    impl_->driver.stdoutFd =
        _open_osfhandle(reinterpret_cast<intptr_t>(hStdoutRead), 0);
    impl_->driver.name = driver_name;
}
#else
void StandAloneComponent::startUnixProcess(const std::string& driver_name,
                                           int stdinPipe[2],
                                           int stdoutPipe[2]) {
    auto [shmFd, shmPtr] =
        createSharedMemory().value_or(std::pair<int, int*>{-1, nullptr});
    if (shmFd == -1 || shmPtr == nullptr) {
        impl_->closePipes(stdinPipe, stdoutPipe);
        return;
    }

    auto sem = createSemaphore().value_or(nullptr);
    if (sem == nullptr) {
        impl_->closePipes(stdinPipe, stdoutPipe);
        closeSharedMemory(shmFd, shmPtr);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        handleChildProcess(driver_name, stdinPipe, stdoutPipe, shmPtr, sem,
                           shmFd);
    } else if (pid > 0) {
        handleParentProcess(pid, stdinPipe, stdoutPipe, shmPtr, sem, shmFd);
    } else {
        LOG_F(ERROR, "Failed to fork driver process");
        impl_->closePipes(stdinPipe, stdoutPipe);
        closeSharedMemory(shmFd, shmPtr);
        sem_close(sem);
    }
}

auto StandAloneComponent::createSharedMemory()
    -> std::optional<std::pair<int, int*> > {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        LOG_F(ERROR, "Failed to create shared memory");
        return std::nullopt;
    }

    if (ftruncate(shm_fd, sizeof(int)) == -1) {
        LOG_F(ERROR, "Failed to set size of shared memory");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return std::nullopt;
    }

    int* shm_ptr = static_cast<int*>(mmap(
        nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm_ptr == MAP_FAILED) {
        LOG_F(ERROR, "Failed to map shared memory");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return std::nullopt;
    }

    *shm_ptr = 0;  // Initialize shared memory to 0
    return std::make_pair(shm_fd, shm_ptr);
}

void StandAloneComponent::closeSharedMemory(int shm_fd, int* shm_ptr) {
    munmap(shm_ptr, sizeof(int));
    close(shm_fd);
    shm_unlink(SHM_NAME);
}

auto StandAloneComponent::createSemaphore() -> std::optional<sem_t*> {
    sem_t* sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0644, 0);
    if (sem == SEM_FAILED) {
        LOG_F(ERROR, "Failed to create semaphore");
        return std::nullopt;
    }
    sem_unlink(SEM_NAME);  // Ensure the semaphore is removed once it's no
                           // longer needed
    return sem;
}

void StandAloneComponent::handleChildProcess(const std::string& driver_name,
                                             int stdinPipe[2],
                                             int stdoutPipe[2], int* shm_ptr,
                                             sem_t* sem, int shm_fd) {
    close(stdinPipe[1]);
    close(stdoutPipe[0]);

    dup2(stdinPipe[0], STDIN_FILENO);
    dup2(stdoutPipe[1], STDOUT_FILENO);

    execlp(driver_name.data(), driver_name.data(), nullptr);

    *shm_ptr = -1;
    sem_post(sem);
    LOG_F(ERROR, "Failed to exec driver process");
    close(shm_fd);
    munmap(shm_ptr, sizeof(int));
    sem_close(sem);
    std::exit(1);
}

void StandAloneComponent::handleParentProcess(pid_t pid, int stdinPipe[2],
                                              int stdoutPipe[2], int* shm_ptr,
                                              sem_t* sem, int shm_fd) {
    close(stdinPipe[0]);
    close(stdoutPipe[1]);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    if (sem_timedwait(sem, &ts) == -1) {
        LOG_F(ERROR, errno == ETIMEDOUT ? "Driver process start timed out"
                                        : "Failed to wait on semaphore");
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    } else if (*shm_ptr == -1) {
        LOG_F(ERROR, "Driver process failed to start");
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        waitpid(pid, nullptr, 0);
    } else {
        impl_->driver.processHandle = pid;
        impl_->driver.stdinFd = stdinPipe[1];
        impl_->driver.stdoutFd = stdoutPipe[0];
        // impl_->driver.name = driver_name;
    }

    closeSharedMemory(shm_fd, shm_ptr);
    sem_close(sem);
}
#endif

void StandAloneComponent::stopLocalDriver() {
    if (impl_->driver.stdinFd != -1)
        close(impl_->driver.stdinFd);
    if (impl_->driver.stdoutFd != -1)
        close(impl_->driver.stdoutFd);

#if defined(_WIN32) || defined(_WIN64)
    TerminateProcess(reinterpret_cast<HANDLE>(impl_->driver.processHandle), 0);
    CloseHandle(reinterpret_cast<HANDLE>(impl_->driver.processHandle));
#else
    kill(impl_->driver.processHandle, SIGTERM);
    waitpid(impl_->driver.processHandle, nullptr, 0);
#endif
    impl_->shouldExit = true;
    if (impl_->driverThread.joinable()) {
        impl_->driverThread.join();
    }
}

void StandAloneComponent::monitorDrivers() {
#if defined(_WIN32) || defined(_WIN64)
    DWORD exitCode;
    if (GetExitCodeProcess(
            reinterpret_cast<HANDLE>(impl_->driver.processHandle), &exitCode) &&
        exitCode == STILL_ACTIVE) {
        return;
    }
#else
    int status;
    pid_t result = waitpid(impl_->driver.processHandle, &status, WNOHANG);
    if (result == 0)
        return;
    if (result == -1) {
        LOG_F(ERROR, "Failed to wait for driver process");
        return;
    }
#endif
    LOG_F(INFO, "Driver {} exited, restarting...", impl_->driver.name);
    startLocalDriver(impl_->driver.name);
}

void StandAloneComponent::processMessages() {
    std::array<char, 1024> buffer;
    if (impl_->driver.isListening) {
#if defined(_WIN32) || defined(_WIN64)
        int bytesRead = _read(impl_->driver.stdoutFd, buffer.data(),
                              static_cast<unsigned int>(buffer.size()));
#else
        int flags = fcntl(impl_->driver.stdoutFd, F_GETFL, 0);
        fcntl(impl_->driver.stdoutFd, F_SETFL, flags | O_NONBLOCK);
        int bytesRead =
            read(impl_->driver.stdoutFd, buffer.data(), buffer.size());
#endif
        if (bytesRead > 0) {
            impl_->handleDriverOutput(impl_->driver.name,
                                      std::span(buffer.data(), bytesRead));
        }
    }
}

void StandAloneComponent::sendMessageToDriver(std::string_view message) {
#if defined(_WIN32) || defined(_WIN64)
    _write(impl_->driver.stdinFd, message.data(),
           static_cast<unsigned int>(message.size()));
#else
    write(impl_->driver.stdinFd, message.data(), message.size());
#endif
}

void StandAloneComponent::printDriver() const {
    LOG_F(INFO, "{} (PID: {}) {}", impl_->driver.name,
          impl_->driver.processHandle,
          impl_->driver.isListening ? "[Listening]" : "");
}

void StandAloneComponent::toggleDriverListening() {
    impl_->driver.isListening = !impl_->driver.isListening;
    LOG_F(INFO, "Driver {} listening status: {}", impl_->driver.name,
          impl_->driver.isListening ? "ON" : "OFF");
}
