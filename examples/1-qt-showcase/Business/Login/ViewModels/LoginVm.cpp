#include "LoginVm.h"

#include <chrono>
#include <random>
#include <stdexcept>
#include <thread>

namespace showcase::login {

namespace {

aria::async::Task<LoginResult> fake_login(std::string u, std::string p) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> d(0, 99);
    if (p.empty())   throw std::runtime_error("密码不能为空");
    if (u == "bob")  throw std::runtime_error("用户 bob 已被封禁");
    if (d(rng) < 40) throw std::runtime_error("用户名或密码错误");
    co_return LoginResult{"欢迎回来，" + u + "!"};
}

}  // namespace

LoginVm::LoginVm(aria::async::IExecutor& ui, aria::async::IExecutor& worker)
    : login(ui, worker, &fake_login)
{
    scope_.attach(*this);
}

void LoginVm::submit() {
    if (!is_active().get()) return;
    login.execute(username.get(), password.get());
}

}  // namespace showcase::login
