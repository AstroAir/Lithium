
#ifndef HEAL_SERVICE_JWT_AUTHSERVICE_HPP
#define HEAL_SERVICE_JWT_AUTHSERVICE_HPP

#include "auth/JWT.hpp"

#include "data/AuthDto.hpp"
#include "data/PageDto.hpp"
#include "data/SignInDto.hpp"
#include "data/SignUpDto.hpp"
#include "data/StatusDto.hpp"
#include "database/UserDb.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

class AuthService {
private:
    typedef oatpp::web::protocol::http::Status Status;

private:
    OATPP_COMPONENT(std::shared_ptr<UserDb>,
                    m_database);                   // Inject database component
    OATPP_COMPONENT(std::shared_ptr<JWT>, m_jwt);  // Inject JWT component
public:
    oatpp::Object<AuthDto> signUp(const oatpp::Object<SignUpDto>& dto);
    oatpp::Object<AuthDto> signIn(const oatpp::Object<SignInDto>& dto);
    oatpp::Object<StatusDto> deleteUserById(const oatpp::String& id);
};

#endif  // HEAL_SERVICE_JWT_AUTHSERVICE_HPP
