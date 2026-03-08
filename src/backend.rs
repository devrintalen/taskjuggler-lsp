use dashmap::DashMap;
use tower_lsp::jsonrpc::Result;
use tower_lsp::lsp_types::*;
use tower_lsp::{Client, LanguageServer};

use crate::completion;
use crate::hover;
use crate::parser::{self, ParseResult, Symbol};
use crate::semantic_tokens;
use crate::signature;

pub struct Backend {
    client: Client,
    documents: DashMap<Url, ParseResult>,
    texts: DashMap<Url, String>,
}

impl Backend {
    pub fn new(client: Client) -> Self {
        Self {
            client,
            documents: DashMap::new(),
            texts: DashMap::new(),
        }
    }

    async fn on_change(&self, uri: Url, text: String) {
        let result = parser::parse(&text);
        let diagnostics = result.diagnostics.clone();
        self.documents.insert(uri.clone(), result);
        self.texts.insert(uri.clone(), text);
        self.client
            .publish_diagnostics(uri, diagnostics, None)
            .await;
    }
}

fn to_document_symbol(sym: &Symbol) -> DocumentSymbol {
    #[allow(deprecated)]
    DocumentSymbol {
        name: sym.name.clone(),
        detail: Some(sym.detail.clone()),
        kind: sym.kind,
        tags: None,
        deprecated: None,
        range: sym.range,
        selection_range: sym.selection_range,
        children: if sym.children.is_empty() {
            None
        } else {
            Some(sym.children.iter().map(to_document_symbol).collect())
        },
    }
}

#[tower_lsp::async_trait]
impl LanguageServer for Backend {
    async fn initialize(&self, _params: InitializeParams) -> Result<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Kind(
                    TextDocumentSyncKind::FULL,
                )),
                document_symbol_provider: Some(OneOf::Left(true)),
                hover_provider: Some(HoverProviderCapability::Simple(true)),
                signature_help_provider: Some(SignatureHelpOptions {
                    trigger_characters: Some(vec![" ".to_string()]),
                    retrigger_characters: None,
                    work_done_progress_options: Default::default(),
                }),
                completion_provider: Some(CompletionOptions {
                    trigger_characters: Some(vec![",".to_string(), " ".to_string()]),
                    resolve_provider: Some(false),
                    work_done_progress_options: Default::default(),
                    all_commit_characters: None,
                    completion_item: None,
                }),
                semantic_tokens_provider: Some(
                    SemanticTokensServerCapabilities::SemanticTokensOptions(
                        SemanticTokensOptions {
                            legend: semantic_tokens::legend(),
                            full: Some(SemanticTokensFullOptions::Bool(true)),
                            ..Default::default()
                        },
                    ),
                ),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    async fn initialized(&self, _params: InitializedParams) {
        self.client
            .log_message(MessageType::INFO, "taskjuggler-lsp initialized")
            .await;
    }

    async fn shutdown(&self) -> Result<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        self.on_change(params.text_document.uri, params.text_document.text)
            .await;
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        if let Some(change) = params.content_changes.into_iter().last() {
            self.on_change(params.text_document.uri, change.text).await;
        }
    }

    async fn did_close(&self, params: DidCloseTextDocumentParams) {
        let uri = params.text_document.uri;
        self.documents.remove(&uri);
        self.texts.remove(&uri);
        self.client.publish_diagnostics(uri, vec![], None).await;
    }

    async fn document_symbol(
        &self,
        params: DocumentSymbolParams,
    ) -> Result<Option<DocumentSymbolResponse>> {
        let doc_symbols = self
            .documents
            .get(&params.text_document.uri)
            .map(|r| r.symbols.iter().map(to_document_symbol).collect::<Vec<_>>());
        Ok(doc_symbols.map(DocumentSymbolResponse::Nested))
    }

    async fn signature_help(
        &self,
        params: SignatureHelpParams,
    ) -> Result<Option<SignatureHelp>> {
        let uri = &params.text_document_position_params.text_document.uri;
        let pos = params.text_document_position_params.position;

        let Some(text) = self.texts.get(uri) else {
            return Ok(None);
        };
        let Some((kw, arg_count)) = signature::active_context(&text, pos) else {
            return Ok(None);
        };
        Ok(signature::build_signature_help(&kw, arg_count))
    }

    async fn hover(&self, params: HoverParams) -> Result<Option<Hover>> {
        let uri = &params.text_document_position_params.text_document.uri;
        let pos = params.text_document_position_params.position;

        let Some(text) = self.texts.get(uri) else {
            return Ok(None);
        };
        let Some(tok) = hover::token_at(&text, pos) else {
            return Ok(None);
        };
        let Some(docs) = hover::keyword_docs(&tok.text) else {
            return Ok(None);
        };

        Ok(Some(Hover {
            contents: HoverContents::Markup(MarkupContent {
                kind: MarkupKind::Markdown,
                value: docs.to_string(),
            }),
            range: Some(Range {
                start: tok.start,
                end: tok.end,
            }),
        }))
    }

    async fn semantic_tokens_full(
        &self,
        params: SemanticTokensParams,
    ) -> Result<Option<SemanticTokensResult>> {
        let uri = &params.text_document.uri;
        let Some(text) = self.texts.get(uri) else {
            return Ok(None);
        };
        Ok(Some(SemanticTokensResult::Tokens(
            semantic_tokens::build_semantic_tokens(&text),
        )))
    }

    async fn completion(
        &self,
        params: CompletionParams,
    ) -> Result<Option<CompletionResponse>> {
        let uri = &params.text_document_position.text_document.uri;
        let pos = params.text_document_position.position;

        let Some(text) = self.texts.get(uri) else {
            return Ok(None);
        };
        let symbols = self
            .documents
            .get(uri)
            .map(|r| r.symbols.clone())
            .unwrap_or_default();

        let items = completion::completions(&text, pos, &symbols);
        if items.is_empty() {
            return Ok(None);
        }
        Ok(Some(CompletionResponse::List(CompletionList {
            is_incomplete: true,
            items,
        })))
    }
}
