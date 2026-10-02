#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tinyxml2.h>

#include "fatcat/template_import.h"

namespace fatcat::detail {

inline const tinyxml2::XMLElement *read_source_xml(
    tinyxml2::XMLDocument &document, std::optional<std::string_view> xml) {
    if (!xml.has_value()) return nullptr;
    if (document.Parse(xml->data(), xml->size()) != tinyxml2::XML_SUCCESS ||
        document.RootElement() == nullptr) {
        throw TemplateImportError("The 3MF project contains invalid XML");
    }
    return document.RootElement();
}

nlohmann::json read_source_identity(const tinyxml2::XMLElement *source_model,
                                    const tinyxml2::XMLElement *slice_info,
                                    const nlohmann::json &target);

}  // namespace fatcat::detail
