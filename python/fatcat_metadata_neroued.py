"""Apply Fat Cat metadata descriptions to a neroued 3MF builder.

This adapter intentionally contains no slicer rules or package writing. The
writer is imported only when component parts need to be registered.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def apply_root_model_metadata(builder: Any, root_model: Mapping[str, Any]) -> None:
    """Apply the composed root metadata to a writer builder.

    将 composer 返回的根 metadata 应用到 writer 构建器。

    Args:
        builder: A ``neroued_3mf.DocumentBuilder`` instance. neroued 构建器。
        root_model: The ``root_model`` object returned by Fat Cat. Fat Cat 返回的根描述。
    """

    for namespace in root_model.get("namespaces", []):
        builder.add_namespace(namespace["prefix"], namespace["uri"])
    for metadata in root_model.get("metadata", []):
        builder.add_metadata(metadata["name"], metadata["value"])
    for external_metadata in root_model.get("external_metadata", []):
        builder.add_external_model_metadata(
            external_metadata["name"], external_metadata["value"]
        )


def apply_metadata_components(
    builder: Any,
    description: Mapping[str, Any],
    resource_bytes: Mapping[str, bytes],
) -> None:
    """Register Fat Cat-described package parts, content types, and relations.

    将 Fat Cat 描述的部件、content type 和关系登记到调用方的 writer。

    Args:
        builder: A ``neroued_3mf.DocumentBuilder`` instance. neroued 构建器。
        description: The result returned by ``compose_model_metadata``.
            ``compose_model_metadata`` 的返回值。
        resource_bytes: Caller-owned bytes indexed by the resource roles in the
            metadata description. 按元数据资源角色索引的调用方字节数据。
    """

    import neroued_3mf as n3mf

    parts = description.get("parts")
    if not isinstance(parts, list):
        raise RuntimeError("Fat Cat metadata composer returned no parts")
    for part in parts:
        if not isinstance(part, Mapping):
            raise RuntimeError("Fat Cat metadata composer returned an invalid part")
        path = part.get("path")
        media_type = part.get("media_type")
        if not isinstance(path, str) or not isinstance(media_type, str):
            raise RuntimeError("Fat Cat metadata part omitted its path or media type")
        if "content" in part:
            content = part["content"]
            if not isinstance(content, str):
                raise RuntimeError(f"Fat Cat text part {path} has non-text content")
            data = content.encode("utf-8")
        else:
            resource_role = part.get("resource_role")
            if (
                not isinstance(resource_role, str)
                or resource_role not in resource_bytes
            ):
                raise RuntimeError(
                    f"Caller did not provide Fat Cat resource role {resource_role!r}"
                )
            data = resource_bytes[resource_role]
            if not isinstance(data, bytes):
                raise RuntimeError(
                    f"Caller resource role {resource_role!r} is not immutable bytes"
                )
        builder.add_custom_part(n3mf.CustomPart(path, media_type, data))

    content_types = description.get("content_types")
    if not isinstance(content_types, list):
        raise RuntimeError("Fat Cat metadata composer returned no content types")
    for item in content_types:
        if not isinstance(item, Mapping):
            raise RuntimeError(
                "Fat Cat metadata composer returned an invalid content type"
            )
        extension = item.get("extension")
        media_type = item.get("media_type")
        if not isinstance(extension, str) or not isinstance(media_type, str):
            raise RuntimeError(
                "Fat Cat content type omitted its extension or media type"
            )
        builder.add_custom_content_type(n3mf.CustomContentType(extension, media_type))

    relationships = description.get("relationships")
    if not isinstance(relationships, list):
        raise RuntimeError("Fat Cat metadata composer returned no relationships")
    add_relationship = getattr(builder, "add_custom_relationship", None)
    if relationships and not callable(add_relationship):
        raise RuntimeError("3MF builder cannot register Fat Cat metadata relationships")
    for relationship in relationships:
        if not isinstance(relationship, Mapping):
            raise RuntimeError(
                "Fat Cat metadata composer returned an invalid relationship"
            )
        fields = tuple(
            relationship.get(name)
            for name in ("source", "id", "type", "target")
        )
        if any(not isinstance(value, str) for value in fields):
            raise RuntimeError("Fat Cat relationship omitted a required text field")
        add_relationship(n3mf.CustomRelationship(*fields))
