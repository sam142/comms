/*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 * 
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS 
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "util/config_helper.hpp"
#include "util/string_helper.hpp"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "logger.hpp"

namespace comms
{
    static const uint32 kConfigLineMax = 1024;
    void conf_del(Properties& conf, const std::string& name, const std::string& value)
    {
        ConfItemsArray& arrays = conf[name];
        if(!arrays.empty())
        {
            ConfItemsArray::iterator it = arrays.begin();
            while(it != arrays.end())
            {
                if(it->at(0) == value)
                {
                    arrays.erase(it);
                    break;
                }
                it++;
            }
            if(arrays.empty())
            {
                conf.erase(name);
            }
        }
    }

    void conf_set(Properties& conf, const std::string& name, const std::string& value, bool replace)
    {
        ConfItemsArray& arrays = conf[name];
        if(replace)
        {
            arrays.clear();
        }
        ConfItems ar;
        ar.push_back(value);
        arrays.push_back(ar);
    }

    bool parse_conf_file(const std::string& path, Properties& result, const char* sep)
    {
        char buf[kConfigLineMax + 1];
        FILE *fp;
        if ((fp = fopen(path.c_str(), "r")) == NULL)
        {
            return false;
        }
        uint32 lineno = 1;
        while (fgets(buf, kConfigLineMax, fp) != NULL)
        {
            char* line = trim_str(buf, "\r\n\t ");
            if (line[0] == '#' || line[0] == '\0')
            {
                lineno++;
                continue;
            }
            std::vector<char*> sp_ret = split_str(line, sep);
            if (sp_ret.size() < 2)
            {
                ERROR_LOG("Invalid config line at line:%u", lineno);
                fclose(fp);
                return false;
            }
            char* key = trim_str(sp_ret[0], "\r\n\t ");
            ConfItems values;
            for (uint32_t i = 1; i < sp_ret.size(); i++)
            {
                char* value = trim_str(sp_ret[i], "\r\n\t ");
                values.push_back(value);
            }
            result[key].push_back(values);
            lineno++;
        }
        fclose(fp);
        return true;
    }

    bool conf_get_int64(const Properties& conf, const std::string& name, int64& value, bool ignore_nonexist)
    {
        Properties::const_iterator found = conf.find(name);
        if (found == conf.end() || found->second.size() == 0)
        {
            return ignore_nonexist;
        }
        if (string_toint64(found->second[0][0], value))
        {
            return true;
        }
        std::string size_str = string_toupper(found->second[0][0]);
        value = atoll(size_str.c_str());
        if (size_str.find("M") == (size_str.size() - 1) || size_str.find("MB") == (size_str.size() - 2))
        {
            value *= (1024 * 1024); // convert to megabytes
        }
        else if (size_str.find("G") == (size_str.size() - 1) || size_str.find("GB") == (size_str.size() - 2))
        {
            value *= (1024 * 1024 * 1024); // convert to kilobytes
        }
        else if (size_str.find("K") == (size_str.size() - 1) || size_str.find("KB") == (size_str.size() - 2))
        {
            value *= 1024; // convert to kilobytes
        }
        else
        {
            return false;
        }
        return true;
    }
    bool conf_get_string(const Properties& conf, const std::string& name, std::string& value, bool ignore_nonexist)
    {
        Properties::const_iterator found = conf.find(name);
        if (found == conf.end() || found->second.size() == 0)
        {
            return ignore_nonexist;
        }
        value = found->second[0][0];
        return true;
    }

    bool conf_get_bool(const Properties& conf, const std::string& name, bool& value, bool ignore_nonexist)
    {
        Properties::const_iterator found = conf.find(name);
        if (found == conf.end() || found->second.size() == 0)
        {
            return ignore_nonexist;
        }
        if (!strcasecmp(found->second[0][0].c_str(), "true") || !strcasecmp(found->second[0][0].c_str(), "1")
                || !strcasecmp(found->second[0][0].c_str(), "yes"))
        {
            value = true;
        }
        else
        {
            value = false;
        }
        return true;
    }
    bool conf_get_double(const Properties& conf, const std::string& name, double& value, bool ignore_nonexist)
    {
        Properties::const_iterator found = conf.find(name);
        if (found == conf.end() || found->second.size() == 0)
        {
            return ignore_nonexist;
        }
        return string_todouble(found->second[0][0], value);
    }

    bool parse_ini_conf_file(const std::string& path, INIProperties& result, const char* sep)
    {
        char buf[kConfigLineMax + 1];
        FILE *fp;
        if ((fp = fopen(path.c_str(), "r")) == NULL)
        {
            return false;
        }
        uint32 lineno = 1;
        std::string current_tag = "";
        while (fgets(buf, kConfigLineMax, fp) != NULL)
        {
            char* line = trim_str(buf, "\r\n\t ");
            if (line[0] == '#' || line[0] == '\0')
            {
                lineno++;
                continue;
            }
            if (line[0] == '[' && line[strlen(line) - 1] == ']')
            {
                current_tag = std::string(line + 1, strlen(line) - 2);
                lineno++;
                continue;
            }
            std::vector<char*> sp_ret = split_str(line, sep);
            Properties& current_prop = result[current_tag];
            if (sp_ret.size() < 2)
            {
                ERROR_LOG("Invalid config line at line:%u", lineno);
                fclose(fp);
                return false;
            }
            char* key = trim_str(sp_ret[0], "\r\n\t ");
            ConfItems values;
            for (uint32_t i = 1; i < sp_ret.size(); i++)
            {
                char* value = trim_str(sp_ret[i], "\r\n\t ");
                values.push_back(value);
            }
            current_prop[key].push_back(values);
            lineno++;
        }
        fclose(fp);
        return true;
    }

    void replace_env_var(Properties& props)
    {
        Properties::iterator it = props.begin();
        while (it != props.end())
        {
            ConfItemsArray& prop_values = it->second;
            ConfItemsArray::iterator sit = prop_values.begin();
            while (sit != prop_values.end())
            {
                ConfItems::iterator ssit = sit->begin();
                while (ssit != sit->end())
                {
                    std::string& prop_value = *ssit;
                    while (true)
                    {
                        size_t pos = prop_value.find("${");
                        if (pos != std::string::npos)
                        {
                            size_t next_pos = prop_value.find('}', pos + 1);
                            if (next_pos != std::string::npos)
                            {
                                std::string env_key = prop_value.substr(pos + 2, next_pos - pos - 2);
                                char* env_value = getenv(env_key.c_str());
                                if (NULL != env_value)
                                {
                                    prop_value.replace(pos, next_pos - pos + 1, env_value);
                                    // this prop value may contain multiple env var
                                    continue;
                                }
                            }
                        }
                        break;
                    }
                    ssit++;
                }
                sit++;
            }
            it++;
        }
    }

    void replace_env_var(INIProperties& props)
    {
        INIProperties::iterator it = props.begin();
        while (it != props.end())
        {
            Properties& sub_props = it->second;
            replace_env_var(sub_props);
            it++;
        }
    }
}
