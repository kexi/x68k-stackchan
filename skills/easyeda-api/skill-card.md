## Description:

EasyEDA Pro API skill for AI agents working with PCB design, schematic editing, footprint and symbol management, project operations, live EasyEDA debugging, and EasyEDA extension development.

This skill is ready for commercial/non-commercial use.

## Publisher:

[yanranxiaoxi](https://clawhub.ai/user/yanranxiaoxi)

### License/Terms of Use:

MIT-0

## Use Case:

Developers and electronics engineers use this skill to look up EasyEDA API and document-format references, generate extension or automation code, and optionally debug against a running EasyEDA client through a local bridge.

### Deployment Geography for Use:

Global

## Known Risks and Mitigations:

Risk: The local bridge can execute JavaScript in a running EasyEDA workspace without authentication.

Mitigation: Install and start the bridge only when intentional, keep it local, review code before execution, and stop the bridge when not in use.

Risk: Automation may change active PCB, schematic, project, or file state.

Mitigation: Use test projects or backups and require explicit approval before edits, deletes, file access, exports, or ordering-related actions.

Risk: Generated API calls may fail or behave differently because of EasyEDA permissions, project state, or document context.

Mitigation: Check bridge health and target window first, prefer small read-only calls before mutations, and validate results in EasyEDA before saving or exporting.

## Reference(s):

- [ClawHub Skill Page](https://clawhub.ai/yanranxiaoxi/skills/easyeda-api)
- [Publisher Profile](https://clawhub.ai/user/yanranxiaoxi)
- [README](README.md)
- [Skill Instructions](SKILL.md)
- [EasyEDA API Index](references/_index.md)
- [EasyEDA API Quick Reference](references/_quick-reference.md)
- [Extension Development Guide](guide/how-to-start.md)
- [Invoking EasyEDA APIs](guide/invoke-apis.md)
- [Document Source Format Reference](format/index.md)
- [Run API Gateway Extension](https://jlcext.com/item/oshwhub-official/run-api-gateway)

## Skill Output:

**Output Type(s):** [text, markdown, code, shell commands, configuration, guidance]

**Output Format:** [Markdown with inline code blocks, shell commands, configuration snippets, and EasyEDA JavaScript examples]

**Output Parameters:** [1D]

**Other Properties Related to Output:** [May include local HTTP requests to the EasyEDA bridge and code intended for execution in the active EasyEDA client.]

## Skill Version(s):

1.1.13 (source: server release metadata; artifact metadata reports 1.1.17)

## Ethical Considerations:

Users should evaluate whether this skill is appropriate for their environment, review any generated or modified files before relying on them, and apply their organization's safety, security, and compliance requirements before deployment.
