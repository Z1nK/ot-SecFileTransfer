#include <auth/user_directory.hpp>
#include <config/config.hpp>
#include <gtest/gtest.h>

#include <string>

// Built only with WITH_LDAP=OFF.

using confide::common::ErrCode;
using confide::config::AuthCfg;
using confide::config::AuthProvider;
using confide::config::Config;
using confide::config::LogCfg;
using confide::config::RestCfg;
using confide::config::RetentionCfg;
using confide::config::StorageCfg;
using confide::config::TlsCfg;

TEST(LdapDisabled, ProviderLdapFailsAtStartup) {
  AuthCfg auth;
  auth.provider = AuthProvider::ldap;
  auth.ldap.uri = "ldap://localhost:1389";
  auth.ldap.user_base_dn = "ou=people,dc=example,dc=org";
  const Config cfg("siteA", StorageCfg{.root = "/tmp/x"}, RestCfg{},
                   TlsCfg{.enabled = false, .cert_path = {}, .key_path = {}, .ca_path = {}},
                   std::move(auth), LogCfg{}, RetentionCfg{});
  auto directory = confide::auth::make_directory(cfg);
  ASSERT_FALSE(directory.has_value());
  EXPECT_EQ(directory.error().code, ErrCode::validation);
  EXPECT_NE(directory.error().detail.find("WITH_LDAP"), std::string::npos);
}
