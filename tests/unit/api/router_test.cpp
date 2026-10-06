#include <api/router.hpp>

#include <gtest/gtest.h>

#include <string>

using confide::api::Access;
using confide::api::RequestContext;
using confide::api::Route;
using confide::api::Router;
using confide::api::url_decode;
using confide::common::ErrCode;

namespace http = boost::beast::http;
namespace asio = boost::asio;

namespace {

asio::awaitable<void> noop(RequestContext&) {
  co_return;
}

Route route(http::verb method, std::string pattern, Access access = Access::user) {
  return Route{.method = method, .pattern = std::move(pattern), .access = access, .handler = noop};
}

class RouterTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(router_.add(route(http::verb::post, "/transactions")));
    ASSERT_TRUE(router_.add(route(http::verb::get, "/transactions")));
    ASSERT_TRUE(router_.add(route(http::verb::get, "/transactions/{id}")));
    ASSERT_TRUE(router_.add(route(http::verb::post, "/transactions/{id}/commit")));
    ASSERT_TRUE(router_.add(route(http::verb::get, "/transactions/{id}/files")));
    ASSERT_TRUE(router_.add(route(http::verb::put, "/transactions/{id}/files/{path*}")));
    ASSERT_TRUE(router_.add(route(http::verb::get, "/transactions/{id}/files/{path*}")));
  }

  Router router_;
};

}  // namespace

TEST(UrlDecode, PlainAndEscapes) {
  EXPECT_EQ(url_decode("abc").value(), "abc");
  EXPECT_EQ(url_decode("a%20b").value(), "a b");
  EXPECT_EQ(url_decode("%2F%2f").value(), "//");
  EXPECT_EQ(url_decode("a+b").value(), "a+b");
  EXPECT_EQ(url_decode("").value(), "");
}

TEST(UrlDecode, DecodesOnlyOnce) {
  EXPECT_EQ(url_decode("%252e%252e").value(), "%2e%2e");
}

TEST(UrlDecode, RejectsMalformed) {
  EXPECT_EQ(url_decode("%").error().code, ErrCode::validation);
  EXPECT_EQ(url_decode("ab%2").error().code, ErrCode::validation);
  EXPECT_EQ(url_decode("%zz").error().code, ErrCode::validation);
  EXPECT_EQ(url_decode("a%00b").error().code, ErrCode::validation);
}

TEST(RouterAdd, RejectsBadPatterns) {
  Router r;
  EXPECT_EQ(r.add(route(http::verb::get, "")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "no-slash")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "/a//b")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "/a/{}")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "/a/{x")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "/{rest*}/b")).error().code, ErrCode::internal);
  EXPECT_EQ(r.add(route(http::verb::get, "/{x}/{x}")).error().code, ErrCode::internal);

  Route no_handler = route(http::verb::get, "/a");
  no_handler.handler = nullptr;
  EXPECT_EQ(r.add(std::move(no_handler)).error().code, ErrCode::internal);
}

TEST(RouterAdd, RejectsDuplicateRoute) {
  Router r;
  ASSERT_TRUE(r.add(route(http::verb::get, "/a")));
  EXPECT_TRUE(r.add(route(http::verb::post, "/a")));
  EXPECT_EQ(r.add(route(http::verb::get, "/a")).error().code, ErrCode::internal);
}

TEST_F(RouterTest, MatchesLiteral) {
  auto m = router_.match(http::verb::post, "/transactions");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->route->pattern, "/transactions");
  EXPECT_EQ(m->route->method, http::verb::post);
  EXPECT_TRUE(m->params.empty());
}

TEST_F(RouterTest, CapturesParam) {
  auto m = router_.match(http::verb::post, "/transactions/tx-1/commit");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->route->pattern, "/transactions/{id}/commit");
  EXPECT_EQ(m->params.at("id"), "tx-1");
}

TEST_F(RouterTest, CapturesRestWithSlashes) {
  auto m = router_.match(http::verb::put, "/transactions/tx-1/files/report/img/b%20c.png");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->route->pattern, "/transactions/{id}/files/{path*}");
  EXPECT_EQ(m->params.at("id"), "tx-1");
  EXPECT_EQ(m->params.at("path"), "report/img/b c.png");
}

TEST_F(RouterTest, RestNeedsAtLeastOneSegment) {
  auto m = router_.match(http::verb::get, "/transactions/tx-1/files");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->route->pattern, "/transactions/{id}/files");
  EXPECT_FALSE(router_.match(http::verb::put, "/transactions/tx-1/files"));
}

TEST_F(RouterTest, EncodedSlashDoesNotSplitSegment) {
  auto m = router_.match(http::verb::get, "/transactions/a%2Fb");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->params.at("id"), "a/b");
}

TEST_F(RouterTest, ParsesQuery) {
  auto m = router_.match(http::verb::get, "/transactions?box=out&x=%41&flag&x=2");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->query.at("box"), "out");
  EXPECT_EQ(m->query.at("x"), "2");
  EXPECT_EQ(m->query.at("flag"), "");
}

TEST_F(RouterTest, UnknownPathIsNotFound) {
  EXPECT_EQ(router_.match(http::verb::get, "/nope").error().code, ErrCode::notfound);
  EXPECT_EQ(router_.match(http::verb::get, "/transactions/").error().code, ErrCode::notfound);
  EXPECT_EQ(router_.match(http::verb::get, "/").error().code, ErrCode::notfound);
}

TEST_F(RouterTest, WrongMethodIsConflict) {
  EXPECT_EQ(router_.match(http::verb::delete_, "/transactions").error().code, ErrCode::conflict);
  EXPECT_EQ(router_.match(http::verb::get, "/transactions/x/commit").error().code,
            ErrCode::conflict);
}

TEST_F(RouterTest, BadEscapeIsValidation) {
  EXPECT_EQ(router_.match(http::verb::get, "/transactions/%zz").error().code,
            ErrCode::validation);
  EXPECT_EQ(router_.match(http::verb::get, "/transactions?box=%").error().code,
            ErrCode::validation);
  EXPECT_EQ(router_.match(http::verb::get, "transactions").error().code, ErrCode::validation);
}
