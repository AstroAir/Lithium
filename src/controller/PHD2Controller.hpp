/*
 * PHD2Controller.cpp
 *
 * Copyright (C) 2023 Max Qian <lightapt.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*************************************************

Copyright: 2023 Max Qian. All rights reserved

Author: Max Qian

E-mail: astro_air@126.com

Date: 2023-7-13

Description: PHD2 Route

**************************************************/

#ifndef Lithium_PHD2CONTROLLER_HPP
#define Lithium_PHD2CONTROLLER_HPP

#include "LithiumApp.hpp"

#include "config.h"

#include "data/PHD2Dto.hpp"
#include "data/StatusDto.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include "atom/type/json.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

class PHD2Controller : public oatpp::web::server::api::ApiController
{
public:
    PHD2Controller(const std::shared_ptr<ObjectMapper> &objectMapper)
        : oatpp::web::server::api::ApiController(objectMapper)
    {
    }

public:
    static std::shared_ptr<PHD2Controller> createShared(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)
    )
    {
        return std::make_shared<PHD2Controller>(objectMapper);
    }

    ENDPOINT_INFO(getUIStartPHD2API)
    {
        info->summary = "Start PHD2 with some parameters";
        info->addConsumes<Object<StartPHD2DTO>>("application/json");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "text/json");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "text/json");
    }
    ENDPOINT_ASYNC("GET", "/api/phd2/start", getUIStartPHD2API){

        ENDPOINT_ASYNC_INIT(getUIStartPHD2API)

        Action act() override
        {
            return request->readBodyToDtoAsync<oatpp::Object<StartPHD2DTO>>(controller->getDefaultObjectMapper()).callbackTo(&getUIStartPHD2API::returnResponse);
        }

        Action returnResponse(const oatpp::Object<StartPHD2DTO>& body)
        {
            auto res = StatusDto::createShared();
            res->command = "StartPHD2";
            res->code = 200;
            auto params = body->phd2_params.getValue("");
            if (params != "")
            {
                try
                {
                    nlohmann::json params_ = nlohmann::json::parse(params);
                    if (!Lithium::MyApp->createProcess("phd2","phd2"))
                    {
                        res->error = "Process Failed";
                        res->message = "Failed to start PHD2";
                    }
                }
                catch (const nlohmann::json::parse_error &e)
                {
                    res->error = "Invalid Parameters";
                    res->message = "Failed to parse PHD2 parameters";
                }
                catch (const std::exception &e)
                {
                    res->error = "PHD2 Failed";
                    res->message = "Unknown error happen when starting PHD2";
                }
            }
            return _return(controller->createDtoResponse(Status::CODE_200,res));
        }
    };

    ENDPOINT_INFO(getUIStopPHD2ParamAPI)
    {
        info->summary = "Stop PHD2";
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "text/json");
    }
    ENDPOINT_ASYNC("GET", "/api/phd2/stop", getUIStopPHD2ParamAPI){

        ENDPOINT_ASYNC_INIT(getUIStopPHD2ParamAPI)

        Action act() override
        {
            auto res = StatusDto::createShared();
            res->command = "StopPHD2";
            if (!Lithium::MyApp->terminateProcessByName("phd2"))
            {
                res->error = "Process Failed";
                res->message = "Failed to stop PHD2";
            }
            return _return(controller->createDtoResponse(Status::CODE_200, res));
        }
    };

    ENDPOINT_INFO(getUIModifyPHD2ParamAPI)
    {
        info->summary = "Modify PHD2 Parameter with name and value";
        info->addConsumes<Object<ModifyPHD2ParamDTO>>("application/json");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "text/json");
    }
    ENDPOINT_ASYNC("GET", "/api/phd2/modify", getUIModifyPHD2ParamAPI){

        ENDPOINT_ASYNC_INIT(getUIModifyPHD2ParamAPI)

        Action act() override
        {
            return request->readBodyToDtoAsync<oatpp::Object<ModifyPHD2ParamDTO>>(controller->getDefaultObjectMapper()).callbackTo(&getUIModifyPHD2ParamAPI::returnResponse);
        }

        Action returnResponse(const oatpp::Object<ModifyPHD2ParamDTO>& body)
        {
            auto res = StatusDto::createShared();
            res->command = "ModifyPHD2Param";
            res->code = 200;
            auto param_name = body->param_name.getValue("");
            auto param_value = body->param_value.getValue("");
            // OATPP_ASSERT_HTTP(param_name && param_value, Status::CODE_400, "parameter name and id should not be null");
            bool phd2_running = false;
            for (auto process : Lithium::MyApp->getRunningProcesses())
            {
                if(process.name == "phd2")
                {
                    phd2_running = true;
                }
            }
            if (phd2_running)
            {
                // PHD2参数热更新
            }
            else
            {
                // PHD2参数冷更新，主要是修改配置文件
            }
            return _return(controller->createDtoResponse(Status::CODE_200,res));
        }
    };

};

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen

#endif // Lithium_PHD2CONTROLLER_HPP